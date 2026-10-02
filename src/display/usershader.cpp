/*
** usershader.cpp
**
** This file is part of mkxp (AlexRomanR fork).
**
** mkxp is free software: you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation, either version 2 of the License, or
** (at your option) any later version.
*/

#include "usershader.h"

#include "bitmap.h"
#include "exception.h"
#include "filesystem.h"
#include "gl-util.h"
#include "glstate.h"
#include "graphics.h"
#include "quad.h"
#include "sharedstate.h"

#include <SDL_rwops.h>
#include <SDL_timer.h>

#include <cmath>
#include <cstring>
#include <stdint.h>

/* Defined by shader.cpp (embedded shader/common.h): including the generated
 * .xxd here would define it twice */
extern const uint8_t mkxp_shader_common_h[];
extern const size_t mkxp_shader_common_h_len;

/* Same interface as mkxp-z's simple.vert, plus v_pos (pixel position of the
 * fragment inside the target) */
static const char defaultVertSource[] =
	"uniform mat4 projMat;\n"
	"uniform vec2 texSizeInv;\n"
	"attribute vec2 position;\n"
	"attribute vec2 texCoord;\n"
	"varying vec2 v_texCoord;\n"
	"varying vec2 v_pos;\n"
	"void main()\n"
	"{\n"
	"	gl_Position = projMat * vec4(position, 0.0, 1.0);\n"
	"	v_texCoord = texCoord * texSizeInv;\n"
	"	v_pos = position;\n"
	"}\n";

/* Attribute locations used by Quad (see Shader::Attribute) */
enum { ATTR_POSITION = 0, ATTR_TEXCOORD = 1, ATTR_COLOR = 2 };

static std::string readGameFile(const std::string &path)
{
	SDL_RWops ops;
	shState->fileSystem().openReadRaw(ops, path.c_str());

	Sint64 size = SDL_RWsize(&ops);
	std::string data(size > 0 ? (size_t)size : 0, '\0');

	if (size > 0)
		SDL_RWread(&ops, &data[0], 1, (size_t)size);

	SDL_RWclose(&ops);

	return data;
}

static std::string shaderLog(GLuint object, bool isProgram)
{
	GLint length = 0;

	if (isProgram)
		gl.GetProgramiv(object, GL_INFO_LOG_LENGTH, &length);
	else
		gl.GetShaderiv(object, GL_INFO_LOG_LENGTH, &length);

	if (length <= 1)
		return "(no log)";

	std::string log(length, '\0');

	if (isProgram)
		gl.GetProgramInfoLog(object, length, 0, &log[0]);
	else
		gl.GetShaderInfoLog(object, length, 0, &log[0]);

	/* Drop the trailing NUL */
	log.resize(strlen(log.c_str()));

	return log;
}

static GLuint compileStage(GLenum type, const std::string &body,
                           const std::string &name)
{
	std::string src;

	if (gl.glsles)
		src += "#define GLSLES\n";

	if (type == GL_FRAGMENT_SHADER)
		src += "#define FRAGMENT_SHADER\n";

	src.append((const char*) mkxp_shader_common_h, mkxp_shader_common_h_len);

	/* Make the GLSL log report lines of the game's file, not of the
	 * concatenated source */
	src += "\n#line 1\n";
	src += body;

	GLuint shader = gl.CreateShader(type);
	const GLchar *text = src.c_str();
	GLint length = (GLint) src.size();

	gl.ShaderSource(shader, 1, &text, &length);
	gl.CompileShader(shader);

	GLint ok = 0;
	gl.GetShaderiv(shader, GL_COMPILE_STATUS, &ok);

	if (!ok)
	{
		std::string log = shaderLog(shader, false);
		gl.DeleteShader(shader);

		throw Exception(Exception::MKXPError, "Shader '%s': %s",
		                name.c_str(), log.c_str());
	}

	return shader;
}

UserShader::UserShader(const char *frag, const char *vert)
    : fragPath(frag ? frag : ""),
      vertPath(vert ? vert : ""),
      program(0)
{
	build();
}

UserShader::~UserShader()
{
	dispose();
}

void UserShader::releaseResources()
{
	if (program)
	{
		if (glState.program.get() == program)
			glState.program.set(0);

		gl.DeleteProgram(program);
		program = 0;
	}

	locations.clear();
	textures.clear();
}

void UserShader::build()
{
	std::string fragSource = readGameFile(fragPath);
	std::string vertSource = vertPath.empty() ? std::string(defaultVertSource)
	                                          : readGameFile(vertPath);

	GLuint vs = compileStage(GL_VERTEX_SHADER, vertSource,
	                         vertPath.empty() ? "(default vertex)" : vertPath);
	GLuint fs;

	try
	{
		fs = compileStage(GL_FRAGMENT_SHADER, fragSource, fragPath);
	}
	catch (const Exception &)
	{
		gl.DeleteShader(vs);
		throw;
	}

	GLuint prog = gl.CreateProgram();
	gl.AttachShader(prog, vs);
	gl.AttachShader(prog, fs);

	gl.BindAttribLocation(prog, ATTR_POSITION, "position");
	gl.BindAttribLocation(prog, ATTR_TEXCOORD, "texCoord");
	gl.BindAttribLocation(prog, ATTR_COLOR, "color");

	gl.LinkProgram(prog);

	/* The program keeps them alive while it exists */
	gl.DeleteShader(vs);
	gl.DeleteShader(fs);

	GLint ok = 0;
	gl.GetProgramiv(prog, GL_LINK_STATUS, &ok);

	if (!ok)
	{
		std::string log = shaderLog(prog, true);
		gl.DeleteProgram(prog);

		throw Exception(Exception::MKXPError, "Shader '%s' (link): %s",
		                fragPath.c_str(), log.c_str());
	}

	/* Success: replace the previous program (if any) */
	if (program)
	{
		if (glState.program.get() == program)
			glState.program.set(0);

		gl.DeleteProgram(program);
	}

	program = prog;
	locations.clear();
}

void UserShader::reload()
{
	guardDisposed();
	build();
}

void UserShader::setFloats(const std::string &name, int count,
                           const float *v)
{
	guardDisposed();

	Value &value = values[name];
	value.count = count < 1 ? 1 : (count > 4 ? 4 : count);

	for (int i = 0; i < 4; ++i)
		value.v[i] = i < value.count ? v[i] : 0.0f;
}

void UserShader::setTexture(const std::string &name, Bitmap *bitmap)
{
	guardDisposed();

	if (bitmap)
		textures[name] = bitmap;
	else
		textures.erase(name);
}

GLint UserShader::location(const std::string &name)
{
	std::map<std::string, GLint>::iterator it = locations.find(name);

	if (it != locations.end())
		return it->second;

	GLint loc = gl.GetUniformLocation(program, name.c_str());
	locations[name] = loc;

	return loc;
}

int UserShader::bindInputs(TEXFBO *source, const Vec2i &targetSize,
                           const Vec2i &outputSize, bool smooth)
{
	GLint loc;

	gl.ActiveTexture(GL_TEXTURE0);

	if (source)
	{
		TEX::bind(source->tex);

		if (smooth)
		{
			TEX::setSmooth(true);
			smoothed.push_back(std::make_pair(0, source->tex.gl));
		}

		if ((loc = location("texture")) >= 0)
			gl.Uniform1i(loc, 0);
		if ((loc = location("texSizeInv")) >= 0)
			gl.Uniform2f(loc, 1.0f / source->width, 1.0f / source->height);
		if ((loc = location("u_texel")) >= 0)
			gl.Uniform2f(loc, 1.0f / source->width, 1.0f / source->height);
		if ((loc = location("u_source_size")) >= 0)
			gl.Uniform2f(loc, (float) source->width, (float) source->height);
	}
	else if ((loc = location("texSizeInv")) >= 0)
	{
		gl.Uniform2f(loc, 1.0f, 1.0f);
	}

	/* Extra textures (set_texture) on units 1..7 */
	int unit = 1;

	for (std::map<std::string, Bitmap*>::iterator it = textures.begin();
	     it != textures.end() && unit < 8; ++it)
	{
		Bitmap *bitmap = it->second;

		if (!bitmap || bitmap->isDisposed())
			continue;

		TEXFBO &tf = bitmap->getGLTypes();

		gl.ActiveTexture(GL_TEXTURE0 + unit);
		TEX::bind(tf.tex);

		if (smooth)
		{
			TEX::setSmooth(true);
			smoothed.push_back(std::make_pair(unit, tf.tex.gl));
		}

		if ((loc = location(it->first)) >= 0)
			gl.Uniform1i(loc, unit);
		if ((loc = location(it->first + "_size")) >= 0)
			gl.Uniform2f(loc, (float) tf.width, (float) tf.height);

		++unit;
	}

	gl.ActiveTexture(GL_TEXTURE0);

	/* Automatic uniforms (only if the shader declares them) */
	if ((loc = location("u_resolution")) >= 0)
		gl.Uniform2f(loc, (float) outputSize.x, (float) outputSize.y);
	if ((loc = location("u_target_size")) >= 0)
		gl.Uniform2f(loc, (float) targetSize.x, (float) targetSize.y);
	if ((loc = location("u_time")) >= 0) /* wrapped: mediump floats on GLES */
		gl.Uniform1f(loc, (float) std::fmod(SDL_GetTicks() / 1000.0, 3600.0));
	if ((loc = location("u_frame")) >= 0)
		gl.Uniform1f(loc, (float) (shState->graphics().getFrameCount() % 100000));

	/* Values set from Ruby */
	for (std::map<std::string, Value>::iterator it = values.begin();
	     it != values.end(); ++it)
	{
		if ((loc = location(it->first)) < 0)
			continue;

		const float *v = it->second.v;

		switch (it->second.count)
		{
		case 1: gl.Uniform1f(loc, v[0]); break;
		case 2: gl.Uniform2f(loc, v[0], v[1]); break;
		case 3: gl.Uniform3f(loc, v[0], v[1], v[2]); break;
		default: gl.Uniform4f(loc, v[0], v[1], v[2], v[3]); break;
		}
	}

	return unit;
}

void UserShader::restoreInputs()
{
	for (size_t i = 0; i < smoothed.size(); ++i)
	{
		gl.ActiveTexture(GL_TEXTURE0 + smoothed[i].first);
		TEX::bind(TEX::ID(smoothed[i].second));
		TEX::setSmooth(false);
	}

	smoothed.clear();
	gl.ActiveTexture(GL_TEXTURE0);
}

void UserShader::draw(TEXFBO &target, const IntRect &dstRect, TEXFBO *source,
                      int blend, bool smooth, const UniformList *overrides)
{
	guardDisposed();

	FBO::ID previous = FBO::boundFramebufferID;
	FBO::bind(target.fbo);

	glState.viewport.pushSet(IntRect(0, 0, target.width, target.height));
	glState.scissorTest.pushSet(false);
	glState.program.set(program);

	/* glOrtho replacement, as ShaderBase::GLProjMat */
	GLint loc = location("projMat");
	if (loc >= 0)
	{
		const float a = 2.f / target.width;
		const float b = 2.f / target.height;
		GLfloat mat[16] =
		{
			 a,  0,  0,  0,
			 0,  b,  0,  0,
			 0,  0, -2,  0,
			-1, -1, -1,  1
		};
		gl.UniformMatrix4fv(loc, 1, GL_FALSE, mat);
	}

	bindInputs(source, Vec2i(target.width, target.height),
	           Vec2i(dstRect.w, dstRect.h), smooth);

	/* Per-draw values (render passes): only for this draw. The stored value
	 * (if any) is re-applied on the next draw by bindInputs */
	if (overrides)
	{
		for (size_t i = 0; i < overrides->size(); ++i)
		{
			GLint l = location((*overrides)[i].first);
			const std::vector<float> &v = (*overrides)[i].second;

			if (l < 0 || v.empty())
				continue;

			switch (v.size())
			{
			case 1: gl.Uniform1f(l, v[0]); break;
			case 2: gl.Uniform2f(l, v[0], v[1]); break;
			case 3: gl.Uniform3f(l, v[0], v[1], v[2]); break;
			default: gl.Uniform4f(l, v[0], v[1], v[2], v[3]); break;
			}
		}
	}

	if (blend < 0)
	{
		glState.blend.pushSet(false);
	}
	else
	{
		glState.blend.pushSet(true);

		if (blend == 3)
		{
			/* Multiply: dst = dst * src (alpha untouched) */
			glState.blendMode.pushSet(BlendNormal);
			gl.BlendEquation(GL_FUNC_ADD);
			gl.BlendFuncSeparate(GL_DST_COLOR, GL_ZERO, GL_ZERO, GL_ONE);
		}
		else
		{
			glState.blendMode.pushSet((BlendType) blend);
		}
	}

	Quad &quad = shState->gpQuad();
	FloatRect texRect = source ? FloatRect(0, 0, source->width, source->height)
	                           : FloatRect(0, 0, 1, 1);
	quad.setTexPosRect(texRect, FloatRect(dstRect));
	quad.setColor(Vec4(1, 1, 1, 1));
	quad.draw();

	if (blend >= 0)
	{
		/* The multiply mode was set behind glState's back: re-apply the
		 * cached mode before popping so the real GL state matches again */
		if (blend == 3)
			glState.blendMode.refresh();

		glState.blendMode.pop();
	}

	glState.blend.pop();

	restoreInputs();

	glState.scissorTest.pop();
	glState.viewport.pop();

	FBO::bind(previous);
}

TEXFBO &UserShader::scratch(int width, int height)
{
	static std::map<std::pair<int, int>, TEXFBO> pool;

	std::pair<int, int> key(width, height);
	std::map<std::pair<int, int>, TEXFBO>::iterator it = pool.find(key);

	if (it != pool.end())
		return it->second;

	TEXFBO &tf = pool[key];
	TEXFBO::init(tf);
	TEXFBO::allocEmpty(tf, width, height);
	TEXFBO::linkFBO(tf);

	return tf;
}
