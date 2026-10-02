/*
** usershader.h
**
** This file is part of mkxp (AlexRomanR fork).
**
** GLSL shaders written by the game and driven from Ruby (class Shader):
**   Shader.new(frag_path[, vert_path]), #set, #set_texture, #reload,
**   Bitmap#shade, Viewport#render_target / #render_passes.
**
** Shaders follow the conventions of mkxp-z's own shaders so they also run on
** OpenGL ES 2.0 (Android): GLSL ES 1.00 style (attribute/varying/texture2D/
** gl_FragColor), no #version line, common.h prepended (precision for GLES).
**
** mkxp is free software: you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation, either version 2 of the License, or
** (at your option) any later version.
*/

#ifndef USERSHADER_H
#define USERSHADER_H

#include "disposable.h"
#include "etc-internal.h"
#include "gl-fun.h"

#include <map>
#include <string>
#include <vector>

class Bitmap;
struct TEXFBO;

/* Uniform values for a single draw (render pass), applied on top of the ones
 * set with setFloats: (name, 1-4 floats) */
typedef std::vector<std::pair<std::string, std::vector<float> > > UniformList;

class UserShader : public Disposable
{
public:
	/* vertPath may be null/empty: the default vertex shader is used */
	UserShader(const char *fragPath, const char *vertPath);
	~UserShader();

	/* Recompile from the same files. If it fails, the previous program is
	 * kept and an Exception with the GLSL log (file and line) is thrown. */
	void reload();

	/* Uniform values set from Ruby; applied every time the shader draws */
	void setFloats(const std::string &name, int count, const float *values);
	void setTexture(const std::string &name, Bitmap *bitmap);

	const std::string &getFragPath() const { return fragPath; }
	const std::string &getVertPath() const { return vertPath; }

	/* Draws a quad covering dstRect of `target` (an FBO of targetSize),
	 * sampling `source` (unit 0, uniform "texture") across the whole quad.
	 * blend: -1 replace, 0 normal (alpha), 1 add, 2 subtract, 3 multiply.
	 * smooth: bilinear filtering on every input texture. */
	void draw(TEXFBO &target, const IntRect &dstRect, TEXFBO *source,
	          int blend, bool smooth, const UniformList *overrides = 0);

	/* Exact-size scratch FBO (shading a bitmap onto itself needs a copy) */
	static TEXFBO &scratch(int width, int height);

private:
	void releaseResources();
	const char *klassName() const { return "shader"; }

	void build();
	GLint location(const std::string &name);
	int bindInputs(TEXFBO *source, const Vec2i &targetSize,
	               const Vec2i &outputSize, bool smooth);
	void restoreInputs();

	std::string fragPath, vertPath;
	GLuint program;

	struct Value
	{
		int count;
		float v[4];
	};

	std::map<std::string, Value> values;
	std::map<std::string, Bitmap*> textures;
	std::map<std::string, GLint> locations;

	/* Textures switched to bilinear for one draw, restored afterwards */
	std::vector<std::pair<int, unsigned int> > smoothed;
};

#endif // USERSHADER_H
