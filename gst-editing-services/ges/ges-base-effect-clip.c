/* GStreamer Editing Services
 * Copyright (C) 2011 Thibault Saunier <thibault.saunier@collabora.co.uk>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

/**
 * SECTION: gesbaseeffectclip
 * @short_description: An effect in a GESLayer
 *
 * The effect will be applied on the sources that have lower priorities
 * (higher number) between the inpoint and the end of it.
 */

#include <ges/ges.h>
#include "ges-internal.h"
#include "ges-types.h"

G_DEFINE_ABSTRACT_TYPE (GESBaseEffectClip, ges_base_effect_clip,
    GES_TYPE_OPERATION_CLIP);

struct _GESBaseEffectClipPrivate
{
  void *nothing;
};


static void
ges_base_effect_clip_class_init (GESBaseEffectClipClass * klass)
{
  g_type_class_add_private (klass, sizeof (GESBaseEffectClipPrivate));

}

static void
ges_base_effect_clip_init (GESBaseEffectClip * self)
{
  self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self,
      GES_TYPE_BASE_EFFECT_CLIP, GESBaseEffectClipPrivate);

}
