/*
** shader-binding.cpp
**
** This file is part of mkxp (AlexRomanR fork).
**
** Ruby class Shader (see src/display/usershader.h):
**   Shader.new(frag_path[, vert_path])
**   shader.set(name, x[, y[, z[, w]]])   / shader.set(name, [x, y, ...])
**   shader.set_texture(name, bitmap_or_nil)
**   shader.reload                         (raises ShaderError with the GLSL log, keeps the old program)
**   ShaderError < StandardError           (compile/link/missing file errors)
**   shader.frag_path, shader.vert_path
**   shader.dispose, shader.disposed?
**
** mkxp is free software: you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation, either version 2 of the License, or
** (at your option) any later version.
*/

#include "binding-util.h"
#include "binding-types.h"
#include "disposable-binding.h"
#include "sharedstate.h"
#include "usershader.h"
#include "bitmap.h"

#include <string>

#if RAPI_FULL > 187
DEF_TYPE_CUSTOMNAME(UserShader, "Shader");
#else
DEF_ALLOCFUNC_CUSTOMFREE(UserShader, freeInstance<UserShader>);
#endif

/* ShaderError < StandardError: a plain `rescue => e` catches GLSL errors
 * (mkxp's MKXPError derives from Exception) */
static VALUE shaderErrorClass = Qnil;

/* No C++ object may be alive when Ruby raises (longjmp): build the message
 * VALUE here and raise in the caller */
static VALUE tryCreate(const char *frag, const char *vert, UserShader **out)
{
	GFX_LOCK;
	try
	{
		*out = new UserShader(frag, vert);
	}
	catch (const Exception &e)
	{
		GFX_UNLOCK;
		return rb_str_new(e.msg.c_str(), e.msg.size());
	}
	GFX_UNLOCK;

	return Qnil;
}

static VALUE tryReload(UserShader *shader)
{
	GFX_LOCK;
	try
	{
		shader->reload();
	}
	catch (const Exception &e)
	{
		GFX_UNLOCK;
		return rb_str_new(e.msg.c_str(), e.msg.size());
	}
	GFX_UNLOCK;

	return Qnil;
}

static void raiseShaderError(VALUE message)
{
	rb_exc_raise(rb_class_new_instance(1, &message, shaderErrorClass));
}

static std::string uniformName(VALUE name)
{
	if (SYMBOL_P(name))
		name = rb_sym2str(name);

	if (!RB_TYPE_P(name, RUBY_T_STRING))
		rb_raise(rb_eTypeError, "uniform name must be a String or Symbol");

	return std::string(RSTRING_PTR(name), RSTRING_LEN(name));
}

RB_METHOD(shaderInitialize) {
	const char *frag = 0;
	const char *vert = 0;

	rb_get_args(argc, argv, "z|z", &frag, &vert RB_ARG_END);

	UserShader *s = 0;
	VALUE error = tryCreate(frag, vert, &s);

	if (!NIL_P(error))
		raiseShaderError(error);

	setPrivateData(self, s);

	/* Keeps the bitmaps given to set_texture alive */
	rb_iv_set(self, "textures", rb_hash_new());

	return self;
}

RB_METHOD_GUARD(shaderSet) {
	UserShader *s = getPrivateData<UserShader>(self);

	if (argc < 2)
		rb_raise(rb_eArgError, "set(name, x[, y[, z[, w]]])");

	std::string name = uniformName(argv[0]);

	VALUE *vals = argv + 1;
	int count = argc - 1;

	if (count == 1 && RB_TYPE_P(vals[0], RUBY_T_ARRAY))
	{
		VALUE ary = vals[0];
		count = (int) RARRAY_LEN(ary);
		vals = RARRAY_PTR(ary);
	}

	if (count < 1 || count > 4)
		rb_raise(rb_eArgError, "a uniform takes 1 to 4 numbers (got %d)", count);

	float v[4] = { 0, 0, 0, 0 };

	for (int i = 0; i < count; ++i)
		v[i] = (float) NUM2DBL(vals[i]);

	GFX_GUARD_EXC(s->setFloats(name, count, v););

	return self;
}
RB_METHOD_GUARD_END

RB_METHOD_GUARD(shaderSetTexture) {
	UserShader *s = getPrivateData<UserShader>(self);

	VALUE nameObj, bitmapObj;
	rb_get_args(argc, argv, "oo", &nameObj, &bitmapObj RB_ARG_END);

	std::string name = uniformName(nameObj);
	Bitmap *bitmap = 0;

	if (!NIL_P(bitmapObj))
		bitmap = getPrivateDataCheck<Bitmap>(bitmapObj, BitmapType);

	GFX_GUARD_EXC(s->setTexture(name, bitmap););

	rb_hash_aset(rb_iv_get(self, "textures"),
	             rb_str_new(name.c_str(), name.size()), bitmapObj);

	return self;
}
RB_METHOD_GUARD_END

RB_METHOD(shaderReload) {
	RB_UNUSED_PARAM;

	UserShader *s = getPrivateData<UserShader>(self);

	if (s->isDisposed())
		raiseDisposedAccess(self);

	VALUE error = tryReload(s);

	if (!NIL_P(error))
		raiseShaderError(error);

	return self;
}

RB_METHOD(shaderFragPath) {
	RB_UNUSED_PARAM;

	UserShader *s = getPrivateData<UserShader>(self);
	const std::string &path = s->getFragPath();

	return rb_str_new(path.c_str(), path.size());
}

RB_METHOD(shaderVertPath) {
	RB_UNUSED_PARAM;

	UserShader *s = getPrivateData<UserShader>(self);
	const std::string &path = s->getVertPath();

	if (path.empty())
		return Qnil;

	return rb_str_new(path.c_str(), path.size());
}

void shaderBindingInit() {
	shaderErrorClass = rb_define_class("ShaderError", rb_eStandardError);

	VALUE klass = rb_define_class("Shader", rb_cObject);
#if RAPI_FULL > 187
	rb_define_alloc_func(klass, classAllocate<&UserShaderType>);
#else
	rb_define_alloc_func(klass, UserShaderAllocate);
#endif

	disposableBindingInit<UserShader>(klass);

	_rb_define_method(klass, "initialize", shaderInitialize);
	_rb_define_method(klass, "set", shaderSet);
	_rb_define_method(klass, "set_texture", shaderSetTexture);
	_rb_define_method(klass, "reload", shaderReload);
	_rb_define_method(klass, "frag_path", shaderFragPath);
	_rb_define_method(klass, "vert_path", shaderVertPath);
}
