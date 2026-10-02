/*
** viewport.cpp
**
** This file is part of mkxp.
**
** Copyright (C) 2013 - 2021 Amaryllis Kulla <ancurio@mapleshrine.eu>
**
** mkxp is free software: you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation, either version 2 of the License, or
** (at your option) any later version.
**
** mkxp is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with mkxp.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "viewport.h"

#include "sharedstate.h"
#include "etc.h"
#include "util.h"
#include "quad.h"
#include "glstate.h"
#include "graphics.h"
#include "bitmap.h"
#include "usershader.h"
#include "gl-util.h"
#include "exception.h"
#include "debugwriter.h"

#include <SDL_rect.h>

#include "sigslot/signal.hpp"

struct ViewportPrivate
{
	/* Needed for geometry changes */
	Viewport *self;

	Rect *rect;
	sigslot::connection rectCon;

	Color *color;
	Tone *tone;

	IntRect screenRect;
	int isOnScreen;

	/* Render target (AlexRomanR fork) */
	Bitmap *target;
	bool targetClear;
	std::vector<RenderPass> passes;
	sigslot::connection targetDispCon;

	EtcTemps tmp;

	ViewportPrivate(int x, int y, int width, int height, Viewport *self)
	    : self(self),
	      rect(&tmp.rect),
	      color(&tmp.color),
	      tone(&tmp.tone),
	      isOnScreen(false),
	      target(0),
	      targetClear(true)
	{
		rect->set(x, y, width, height);
		updateRectCon();
	}

	~ViewportPrivate()
	{
		rectCon.disconnect();
		targetDispCon.disconnect();
	}

	void onRectChange()
	{
		self->geometry.rect = rect->toIntRect();
		self->notifyGeometryChange();
		recomputeOnScreen();
	}

	void updateRectCon()
	{
		rectCon.disconnect();
		rectCon = rect->valueChanged.connect
		        (&ViewportPrivate::onRectChange, this);
	}

	void recomputeOnScreen()
	{
		SDL_Rect r1 = { screenRect.x, screenRect.y,
		                screenRect.w, screenRect.h };

		SDL_Rect r2 = { rect->x,     rect->y,
		                rect->width, rect->height };

		SDL_Rect result;
		isOnScreen = SDL_IntersectRect(&r1, &r2, &result);
	}

	bool needsEffectRender(bool flashing)
	{
		bool rectEffective = !rect->isEmpty();
		bool colorToneEffective = color->hasEffect() || tone->hasEffect() || flashing;

		return (rectEffective && colorToneEffective && isOnScreen);
	}
};

Viewport::Viewport(int x, int y, int width, int height)
    : SceneElement(*shState->screen()),
      sceneLink(this)
{
	initViewport(x, y, width, height);
}

Viewport::Viewport(Rect *rect)
    : SceneElement(*shState->screen()),
      sceneLink(this)
{
	initViewport(rect->x, rect->y, rect->width, rect->height);
}

Viewport::Viewport()
    : SceneElement(*shState->screen()),
      sceneLink(this)
{
	const Graphics &graphics = shState->graphics();
	initViewport(0, 0, graphics.width(), graphics.height());
}

void Viewport::initViewport(int x, int y, int width, int height)
{
	p = new ViewportPrivate(x, y, width, height, this);

	/* Set our own geometry */
	geometry.rect = IntRect(x, y, width, height);

	/* Handle parent geometry */
	onGeometryChange(scene->getGeometry());
}

Viewport::~Viewport()
{
	dispose();
}

void Viewport::update()
{
	guardDisposed();

	Flashable::update();
}

DEF_ATTR_RD_SIMPLE(Viewport, OX,   int,   geometry.orig.x)
DEF_ATTR_RD_SIMPLE(Viewport, OY,   int,   geometry.orig.y)

DEF_ATTR_SIMPLE(Viewport, Rect,  Rect&,  *p->rect)
DEF_ATTR_SIMPLE(Viewport, Color, Color&, *p->color)
DEF_ATTR_SIMPLE(Viewport, Tone,  Tone&,  *p->tone)

void Viewport::setOX(int value)
{
	guardDisposed();

	if (geometry.orig.x == value)
		return;

	geometry.orig.x = value;
	notifyGeometryChange();
}

void Viewport::setOY(int value)
{
	guardDisposed();

	if (geometry.orig.y == value)
		return;

	geometry.orig.y = value;
	notifyGeometryChange();
}

void Viewport::initDynAttribs()
{
	p->rect = new Rect(*p->rect);
	p->color = new Color;
	p->tone = new Tone;

	p->updateRectCon();
}

/* Scene */
void Viewport::composite()
{
	if (emptyFlashFlag)
		return;

	if (p->target)
	{
		compositeToTarget();
		return;
	}

	bool renderEffect = p->needsEffectRender(flashing);

	if (elements.getSize() == 0 && !renderEffect)
		return;

	/* Setup scissor */
	glState.scissorTest.pushSet(true);
	glState.scissorBox.pushSet(p->rect->toIntRect());

	Scene::composite();

	/* If any effects are visible, request parent Scene to
	 * render them. */
	if (renderEffect)
		scene->requestViewportRender
		        (p->color->norm, flashColor, p->tone->norm);

	glState.scissorBox.pop();
	glState.scissorTest.pop();
}

/* Render target (AlexRomanR fork) */
void Viewport::compositeToTarget()
{
	Bitmap *target = p->target;

	if (target->isDisposed() || target->isMega() || target->isAnimated())
		return;

	TEXFBO &tf = target->getGLTypes();
	const IntRect rect = p->rect->toIntRect();
	const IntRect &screen = scene->getGeometry().rect;

	FBO::ID previous = FBO::boundFramebufferID;
	FBO::bind(tf.fbo);

	/* Children are positioned in screen coordinates: shifting the GL
	 * viewport by -rect.pos puts the viewport origin at the target's 0,0
	 * (the projection uses the screen size, so the mapping stays 1:1) */
	glState.viewport.pushSet(IntRect(-rect.x, -rect.y, screen.w, screen.h));
	glState.scissorTest.pushSet(true);
	glState.scissorBox.pushSet(IntRect(0, 0, std::min(rect.w, tf.width),
	                                   std::min(rect.h, tf.height)));

	if (p->targetClear)
	{
		glState.clearColor.pushSet(Vec4());
		FBO::clear();
		glState.clearColor.pop();
	}

	if (elements.getSize() > 0)
		Scene::composite();

	glState.scissorBox.pop();
	glState.scissorTest.pop();
	glState.viewport.pop();

	FBO::bind(previous);

	/* The target changed on the GPU: drop CPU caches (get_pixel) */
	target->gpuModified(IntRect(0, 0, tf.width, tf.height));

	/* Passes run now, inside the frame, so whatever shows their result
	 * later in this same frame is up to date (no 1-frame lag) */
	for (size_t i = 0; i < p->passes.size(); ++i)
	{
		RenderPass &pass = p->passes[i];

		if (!pass.shader || pass.shader->isDisposed() ||
		    !pass.target || pass.target->isDisposed() ||
		    (pass.source && pass.source->isDisposed()))
			continue;

		try
		{
			pass.target->shade(*pass.shader, pass.source, pass.rect,
			                   pass.blend, pass.smooth);
		}
		catch (const Exception &e)
		{
			/* Never break the frame because of a pass */
			Debug() << "Viewport render pass" << i << "failed:" << e.msg;
		}
	}
}

void Viewport::setRenderTarget(Bitmap *bitmap)
{
	guardDisposed();

	p->targetDispCon.disconnect();
	p->target = bitmap;

	if (bitmap)
	{
		ViewportPrivate *priv = p;
		p->targetDispCon = bitmap->wasDisposed.connect([priv]() { priv->target = 0; });
	}
}

Bitmap *Viewport::getRenderTarget() const
{
	guardDisposed();

	return p->target;
}

bool Viewport::getRenderTargetClear() const
{
	guardDisposed();

	return p->targetClear;
}

void Viewport::setRenderTargetClear(bool value)
{
	guardDisposed();

	p->targetClear = value;
}

void Viewport::setRenderPasses(const std::vector<RenderPass> &passes)
{
	guardDisposed();

	p->passes = passes;
}

/* SceneElement */
void Viewport::draw()
{
	composite();
}

void Viewport::onGeometryChange(const Geometry &geo)
{
	p->screenRect = geo.rect;
	p->recomputeOnScreen();
}

void Viewport::releaseResources()
{
	unlink();

	delete p;
}


ViewportElement::ViewportElement(Viewport *viewport, int z, int spriteY)
    : SceneElement(viewport ? *viewport : *shState->screen(), z, spriteY),
      m_viewport(viewport)
{
	if (rgssVer == 1 && viewport)
		viewportDispCon = viewport->wasDisposed.connect(&ViewportElement::viewportElementDisposal, this);
}

Viewport *ViewportElement::getViewport() const
{
	return m_viewport;
}

void ViewportElement::setViewport(Viewport *viewport)
{
	m_viewport = viewport;
	
	viewportDispCon.disconnect();
	if (rgssVer == 1 && viewport)
		viewportDispCon = viewport->wasDisposed.connect(&ViewportElement::viewportElementDisposal, this);
	
	setScene(viewport ? *viewport : *shState->screen());
	onViewportChange();
	onGeometryChange(scene->getGeometry());
}

void ViewportElement::viewportElementDisposal()
{
	viewportDispCon.disconnect();
	Disposable *self = dynamic_cast<Disposable*>(this);
	if(self != nullptr)
		self->dispose();
}

ViewportElement::~ViewportElement()
{
	viewportDispCon.disconnect();
}
