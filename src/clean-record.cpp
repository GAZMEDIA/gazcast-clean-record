/*
GazCast Clean Record for OBS Studio
Copyright (C) 2026 GazCast

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

/*
 * Clean recording.
 *
 * How it works:
 * - The dock lists every scene and video source, ticked = in the clean
 *   recording. An unticked scene or source carries the "Hide in GazCast clean recording" filter
 *   (gazcast_clean_hide), which can also be added in its Filters window and is
 *   saved with the scene collection. Overlay-type sources (browser, image,
 *   slideshow, text, GazCast's own) are only labelled, never unticked for the
 *   user.
 * - While a clean recording runs, a private source (gazcast_clean_mix) renders
 *   OBS's program scene into its own view/video mix, walking its items (and
 *   nested scenes) itself so no texture from the main output is reused.
 *   During that render a flag is set, and the filter draws nothing. Everywhere else (stream, normal recording, preview, projectors)
 *   the filter passes the source through untouched.
 * - The mix gets its own encoders (copies of the normal recording's encoder
 *   settings when available) and an ffmpeg_muxer output, with the same audio
 *   as the normal recording (its first track).
 *
 * Known limit: while a transition runs (and when an item uses "crop to
 * bounds"), the clean mix falls back to OBS's own drawing, which reuses the
 * main output's textures, so overlays can show for that second. The stream is
 * never affected.
 */

#include "clean-record.hpp"

#include <obs-module.h>
#include <obs.hpp>
#include <graphics/matrix4.h>
#include <graphics/vec2.h>
#include <util/config-file.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/util.hpp>

#include <plugin-support.h>

#include <QCheckBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QLabel>
#include <QListWidget>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QString>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {

constexpr const char *kMixSourceId = "gazcast_clean_mix";
constexpr const char *kHideFilterId = "gazcast_clean_hide";
constexpr const char *kDockId = "gazcast-clean";
constexpr const char *kConfigFile = "clean-record.json";

/* > 0 while the clean mix renders. Rendering only happens on OBS's graphics
 * thread, so a plain counter is enough. */
int g_cleanPass = 0;

/* ------------------------------------------------------------------------- */
/* Source types */

const char *MixGetName(void *)
{
	return "GazCast clean mix";
}

/* Scenes draw nested scenes, and cropped, scaled or blended items, into a
 * texture once per frame. The main output draws first, so the clean mix would
 * get those textures with the overlays still in them. Instead the clean mix
 * walks the program scene itself and draws every item into textures of its
 * own, the same way OBS does. During a transition, or if an item uses "crop to
 * bounds", it falls back to OBS's own drawing. */
struct MixData {
	obs_source_t *source = nullptr;
	gs_samplerstate_t *pointSampler = nullptr;
	std::unordered_map<obs_sceneitem_t *, gs_texrender_t *> textures;
	std::unordered_set<obs_sceneitem_t *> used;
};

struct BlendParams {
	gs_blend_type srcColor;
	gs_blend_type srcAlpha;
	gs_blend_type dstColor;
	gs_blend_type dstAlpha;
	gs_blend_op_type op;
};

/* Same order as enum obs_blending_type, values copied from obs-scene.c. */
const BlendParams kBlendParams[] = {
	{GS_BLEND_ONE, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_OP_ADD},           // normal
	{GS_BLEND_ONE, GS_BLEND_ONE, GS_BLEND_ONE, GS_BLEND_ONE, GS_BLEND_OP_ADD},                           // additive
	{GS_BLEND_ONE, GS_BLEND_ONE, GS_BLEND_ONE, GS_BLEND_ONE, GS_BLEND_OP_REVERSE_SUBTRACT},              // subtract
	{GS_BLEND_ONE, GS_BLEND_ONE, GS_BLEND_INVSRCCOLOR, GS_BLEND_INVSRCALPHA, GS_BLEND_OP_ADD},           // screen
	{GS_BLEND_DSTCOLOR, GS_BLEND_DSTALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_OP_ADD}, // multiply
	{GS_BLEND_ONE, GS_BLEND_ONE, GS_BLEND_ONE, GS_BLEND_ONE, GS_BLEND_OP_MAX},                           // lighten
	{GS_BLEND_ONE, GS_BLEND_ONE, GS_BLEND_ONE, GS_BLEND_ONE, GS_BLEND_OP_MIN},                           // darken
};

/* obs_sceneitem_get_bounds_crop only exists in OBS 31 and later. */
using BoundsCropFn = bool (*)(const obs_sceneitem_t *);

BoundsCropFn GetBoundsCropFn()
{
	static BoundsCropFn fn = [] {
#ifdef _WIN32
		HMODULE obs = GetModuleHandleW(L"obs.dll");
		return obs ? reinterpret_cast<BoundsCropFn>(GetProcAddress(obs, "obs_sceneitem_get_bounds_crop"))
			   : nullptr;
#else
		return reinterpret_cast<BoundsCropFn>(dlsym(RTLD_DEFAULT, "obs_sceneitem_get_bounds_crop"));
#endif
	}();
	return fn;
}

/* 0: no other filters, 1: other filters, 2: hidden from the clean recording */
int CleanFilterState(obs_source_t *source)
{
	int state = 0;
	obs_source_enum_filters(
		source,
		[](obs_source_t *, obs_source_t *filter, void *param) {
			int *out = static_cast<int *>(param);
			if (!obs_source_enabled(filter)) {
				return;
			}
			if (strcmp(obs_source_get_unversioned_id(filter), kHideFilterId) == 0) {
				*out = 2;
			} else if (*out == 0) {
				*out = 1;
			}
		},
		&state);
	return state;
}

std::vector<obs_sceneitem_t *> SceneItems(obs_scene_t *scene)
{
	std::vector<obs_sceneitem_t *> items;
	obs_scene_enum_items(
		scene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
			obs_sceneitem_addref(item);
			static_cast<std::vector<obs_sceneitem_t *> *>(param)->push_back(item);
			return true;
		},
		&items);
	return items;
}

void ReleaseItems(std::vector<obs_sceneitem_t *> &items)
{
	for (obs_sceneitem_t *item : items) {
		obs_sceneitem_release(item);
	}
}

bool CanDrawClean(obs_scene_t *scene)
{
	BoundsCropFn boundsCrop = GetBoundsCropFn();
	std::vector<obs_sceneitem_t *> items = SceneItems(scene);
	bool ok = true;
	for (obs_sceneitem_t *item : items) {
		if (!ok || !obs_sceneitem_visible(item)) {
			continue;
		}
		obs_source_t *source = obs_sceneitem_get_source(item);
		if (CleanFilterState(source) == 2) {
			continue;
		}
		if (boundsCrop && obs_sceneitem_get_bounds_type(item) != OBS_BOUNDS_NONE && boundsCrop(item)) {
			ok = false;
		} else if (obs_scene_t *nested = obs_group_or_scene_from_source(source)) {
			ok = CanDrawClean(nested);
		}
	}
	ReleaseItems(items);
	return ok;
}

void DrawSceneClean(MixData *mix, obs_scene_t *scene);

void DrawContentClean(MixData *mix, obs_source_t *source)
{
	obs_scene_t *scene = obs_group_or_scene_from_source(source);
	if (scene && CleanFilterState(source) == 0) {
		DrawSceneClean(mix, scene);
	} else {
		obs_source_video_render(source);
	}
}

const char *TechName(bool upscale, gs_color_space source, gs_color_space current)
{
	bool currentSdr = current == GS_CS_SRGB || current == GS_CS_SRGB_16F;
	switch (source) {
	case GS_CS_SRGB:
	case GS_CS_SRGB_16F:
		if (current == GS_CS_709_SCRGB)
			return upscale ? "DrawUpscaleMultiply" : "DrawMultiply";
		break;
	case GS_CS_709_EXTENDED:
		if (currentSdr)
			return upscale ? "DrawUpscaleTonemap" : "DrawTonemap";
		if (current == GS_CS_709_SCRGB)
			return upscale ? "DrawUpscaleMultiply" : "DrawMultiply";
		break;
	case GS_CS_709_SCRGB:
		if (currentSdr)
			return upscale ? "DrawUpscaleMultiplyTonemap" : "DrawMultiplyTonemap";
		if (current == GS_CS_709_EXTENDED)
			return upscale ? "DrawUpscaleMultiply" : "DrawMultiply";
		break;
	}
	return upscale ? "DrawUpscale" : "Draw";
}

/* Mirrors render_item_texture in obs-scene.c. */
void DrawItemTexture(MixData *mix, obs_sceneitem_t *item, gs_texture_t *tex, const matrix4 &transform,
		     gs_color_space current, gs_color_space source)
{
	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	obs_scale_type type = obs_sceneitem_get_scale_filter(item);
	uint32_t cx = gs_texture_get_width(tex);
	uint32_t cy = gs_texture_get_height(tex);
	float scaleX = sqrtf(transform.x.x * transform.x.x + transform.x.y * transform.x.y);
	float scaleY = sqrtf(transform.y.x * transform.y.x + transform.y.y * transform.y.y);

	bool upscale = false;
	if (type == OBS_SCALE_POINT) {
		if (!mix->pointSampler) {
			gs_sampler_info info = {};
			info.filter = GS_FILTER_POINT;
			info.address_u = GS_ADDRESS_CLAMP;
			info.address_v = GS_ADDRESS_CLAMP;
			info.address_w = GS_ADDRESS_CLAMP;
			mix->pointSampler = gs_samplerstate_create(&info);
		}
		gs_effect_set_next_sampler(gs_effect_get_param_by_name(effect, "image"), mix->pointSampler);
	} else if (type != OBS_SCALE_DISABLE && (fabsf(scaleX - 1.0f) > 1e-4f || fabsf(scaleY - 1.0f) > 1e-4f)) {
		if (scaleX < 0.5f || scaleY < 0.5f) {
			effect = obs_get_base_effect(OBS_EFFECT_BILINEAR_LOWRES);
		} else if (type == OBS_SCALE_BICUBIC) {
			effect = obs_get_base_effect(OBS_EFFECT_BICUBIC);
		} else if (type == OBS_SCALE_LANCZOS) {
			effect = obs_get_base_effect(OBS_EFFECT_LANCZOS);
		} else if (type == OBS_SCALE_AREA) {
			effect = obs_get_base_effect(OBS_EFFECT_AREA);
			upscale = scaleX >= 1.0f && scaleY >= 1.0f;
		}
		if (gs_eparam_t *param = gs_effect_get_param_by_name(effect, "base_dimension")) {
			vec2 base;
			vec2_set(&base, (float)cx, (float)cy);
			gs_effect_set_vec2(param, &base);
		}
		if (gs_eparam_t *param = gs_effect_get_param_by_name(effect, "base_dimension_i")) {
			vec2 base;
			vec2_set(&base, 1.0f / (float)cx, 1.0f / (float)cy);
			gs_effect_set_vec2(param, &base);
		}
	}

	bool sourceSdr = source != GS_CS_709_SCRGB;
	bool currentSdr = current != GS_CS_709_SCRGB;
	float multiplier = 1.0f;
	if (!currentSdr && sourceSdr) {
		multiplier = obs_get_video_sdr_white_level() / 80.0f;
	} else if (currentSdr && !sourceSdr) {
		multiplier = 80.0f / obs_get_video_sdr_white_level();
	}
	if (gs_eparam_t *param = gs_effect_get_param_by_name(effect, "multiplier")) {
		gs_effect_set_float(param, multiplier);
	}

	size_t mode = (size_t)obs_sceneitem_get_blending_mode(item);
	const BlendParams &blend = kBlendParams[mode < std::size(kBlendParams) ? mode : 0];
	gs_blend_state_push();
	gs_blend_function_separate(blend.srcColor, blend.dstColor, blend.srcAlpha, blend.dstAlpha);
	gs_blend_op(blend.op);
	while (gs_effect_loop(effect, TechName(upscale, source, current))) {
		obs_source_draw(tex, 0, 0, 0, 0, false);
	}
	gs_blend_state_pop();
}

/* Mirrors render_item in obs-scene.c, without show/hide transitions. */
void DrawItemClean(MixData *mix, obs_sceneitem_t *item)
{
	obs_source_t *source = obs_sceneitem_get_source(item);
	if (!obs_sceneitem_visible(item) || CleanFilterState(source) == 2) {
		return;
	}

	obs_sceneitem_crop crop;
	obs_sceneitem_get_crop(item, &crop);
	bool cropped = crop.left || crop.right || crop.top || crop.bottom;
	obs_blending_method method = obs_sceneitem_get_blending_method(item);
	bool useTexture = cropped || obs_sceneitem_get_scale_filter(item) != OBS_SCALE_DISABLE ||
			  method == OBS_BLEND_METHOD_SRGB_OFF ||
			  obs_sceneitem_get_blending_mode(item) != OBS_BLEND_NORMAL ||
			  (obs_scene_from_source(source) != nullptr);

	matrix4 transform;
	obs_sceneitem_get_draw_transform(item, &transform);

	if (!useTexture) {
		bool previous = gs_set_linear_srgb(true);
		gs_matrix_push();
		gs_matrix_mul(&transform);
		DrawContentClean(mix, source);
		gs_matrix_pop();
		gs_set_linear_srgb(previous);
		return;
	}

	uint32_t width = obs_source_get_width(source);
	uint32_t height = obs_source_get_height(source);
	if (!width || !height) {
		return;
	}
	uint32_t cropX = (uint32_t)(crop.left + crop.right);
	uint32_t cropY = (uint32_t)(crop.top + crop.bottom);
	uint32_t cx = cropX > width ? 2 : width - cropX;
	uint32_t cy = cropY > height ? 2 : height - cropY;

	gs_color_space current = gs_get_color_space();
	gs_color_space space = obs_source_get_color_space(source, 1, &current);
	gs_color_format format = gs_get_format_from_space(space);

	gs_texrender_t *&texrender = mix->textures[item];
	if (texrender && gs_texrender_get_format(texrender) != format) {
		gs_texrender_destroy(texrender);
		texrender = nullptr;
	}
	if (!texrender) {
		texrender = gs_texrender_create(format, GS_ZS_NONE);
	}
	mix->used.insert(item);

	gs_texrender_reset(texrender);
	if (!gs_texrender_begin_with_color_space(texrender, cx, cy, space)) {
		return;
	}
	vec4 clear;
	vec4_zero(&clear);
	gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
	gs_ortho(0.0f, (float)width, 0.0f, (float)height, -100.0f, 100.0f);
	gs_matrix_scale3f((float)width / (float)cx, (float)height / (float)cy, 1.0f);
	gs_matrix_translate3f(-(float)crop.left, -(float)crop.top, 0.0f);
	DrawContentClean(mix, source);
	gs_texrender_end(texrender);

	gs_texture_t *tex = gs_texrender_get_texture(texrender);
	if (!tex) {
		return;
	}
	bool previous = gs_set_linear_srgb(method != OBS_BLEND_METHOD_SRGB_OFF);
	gs_matrix_push();
	gs_matrix_mul(&transform);
	DrawItemTexture(mix, item, tex, transform, current, space);
	gs_matrix_pop();
	gs_set_linear_srgb(previous);
}

void DrawSceneClean(MixData *mix, obs_scene_t *scene)
{
	std::vector<obs_sceneitem_t *> items = SceneItems(scene);
	gs_blend_state_push();
	gs_reset_blend_state();
	for (obs_sceneitem_t *item : items) {
		DrawItemClean(mix, item);
	}
	gs_blend_state_pop();
	ReleaseItems(items);
}

/* The scene on air when no transition is running, or null. */
obs_source_t *SteadyProgramScene(obs_source_t *program)
{
	if (obs_source_get_type(program) != OBS_SOURCE_TYPE_TRANSITION) {
		return obs_scene_from_source(program) ? obs_source_get_ref(program) : nullptr;
	}
	obs_source_t *next = obs_transition_get_source(program, OBS_TRANSITION_SOURCE_B);
	if (next) {
		obs_source_release(next);
		return nullptr;
	}
	obs_source_t *scene = obs_transition_get_source(program, OBS_TRANSITION_SOURCE_A);
	if (scene && !obs_scene_from_source(scene)) {
		obs_source_release(scene);
		return nullptr;
	}
	return scene;
}

void *MixCreate(obs_data_t *, obs_source_t *source)
{
	auto *mix = new MixData;
	mix->source = source;
	return mix;
}

void MixDestroy(void *data)
{
	auto *mix = static_cast<MixData *>(data);
	obs_enter_graphics();
	for (auto &entry : mix->textures) {
		gs_texrender_destroy(entry.second);
	}
	if (mix->pointSampler) {
		gs_samplerstate_destroy(mix->pointSampler);
	}
	obs_leave_graphics();
	delete mix;
}

uint32_t MixGetWidth(void *)
{
	obs_video_info ovi;
	return obs_get_video_info(&ovi) ? ovi.base_width : 0;
}

uint32_t MixGetHeight(void *)
{
	obs_video_info ovi;
	return obs_get_video_info(&ovi) ? ovi.base_height : 0;
}

void MixRender(void *data, gs_effect_t *)
{
	auto *mix = static_cast<MixData *>(data);
	obs_source_t *program = obs_get_output_source(0);
	if (!program) {
		return;
	}
	obs_source_t *scene = SteadyProgramScene(program);

	g_cleanPass++;
	if (scene && CleanFilterState(scene) == 2) {
		/* The whole program scene is left out. */
	} else if (scene && CleanFilterState(scene) == 0 && CanDrawClean(obs_scene_from_source(scene))) {
		mix->used.clear();
		DrawSceneClean(mix, obs_scene_from_source(scene));
		for (auto it = mix->textures.begin(); it != mix->textures.end();) {
			if (mix->used.count(it->first)) {
				++it;
			} else {
				gs_texrender_destroy(it->second);
				it = mix->textures.erase(it);
			}
		}
	} else {
		obs_source_video_render(program);
	}
	g_cleanPass--;

	obs_source_release(scene);
	obs_source_release(program);
}

const char *HideGetName(void *)
{
	return obs_module_text("GazCast.Clean.FilterName");
}

void *HideCreate(obs_data_t *, obs_source_t *source)
{
	return source;
}

void HideDestroy(void *) {}

void HideRender(void *data, gs_effect_t *)
{
	if (g_cleanPass > 0) {
		return;
	}
	obs_source_skip_video_filter(static_cast<obs_source_t *>(data));
}

/* ------------------------------------------------------------------------- */
/* Which sources are left out */

bool IsGazCastOverlay(obs_source_t *source)
{
	const char *name = obs_source_get_name(source);
	if (name && strncmp(name, "GazCast", 7) == 0) {
		return true;
	}

	const char *id = obs_source_get_unversioned_id(source);
	OBSDataAutoRelease settings = obs_source_get_settings(source);

	/* GazCast's NDI output, received with the DistroAV (obs-ndi) plugin. NDI
	 * names look like "PC-NAME (GazCast)" or "PC-NAME (GazCast - Lower third)". */
	if (strcmp(id, "ndi_source") == 0) {
		const QString ndiName = QString::fromUtf8(obs_data_get_string(settings, "ndi_source_name"));
		return ndiName.contains(QLatin1String("(GazCast"), Qt::CaseInsensitive);
	}

	if (strcmp(id, "browser_source") != 0) {
		return false;
	}

	/* The app adds overlays as http://127.0.0.1:5510/o/<key>; the web app
	 * serves them from overlays.gazgoc.com.ng. */
	static const QRegularExpression localOverlay(QStringLiteral(R"(^https?://(127\.0\.0\.1|localhost)(:\d+)?/o/)"),
						     QRegularExpression::CaseInsensitiveOption);
	const QString url = QString::fromUtf8(obs_data_get_string(settings, "url"));
	return url.contains(localOverlay) || url.contains(QLatin1String("gazgoc.com.ng"), Qt::CaseInsensitive);
}

/* Graphics laid over the show: web pages, pictures, slideshows and text. Cameras,
 * captures, media files and colour backgrounds are the show itself. */
bool IsOverlay(obs_source_t *source)
{
	static const char *const kOverlayTypes[] = {"browser_source",   "image_source", "slideshow",
						    "slideshow_v2",     "text_gdiplus", "text_ft2_source",
						    "text_pango_source"};
	const char *id = obs_source_get_unversioned_id(source);
	for (const char *type : kOverlayTypes) {
		if (strcmp(id, type) == 0) {
			return true;
		}
	}
	return IsGazCastOverlay(source);
}

/* Returns a new reference, or nullptr. */
obs_source_t *GetHideFilter(obs_source_t *source)
{
	obs_source_t *found = nullptr;
	obs_source_enum_filters(
		source,
		[](obs_source_t *, obs_source_t *filter, void *param) {
			auto *out = static_cast<obs_source_t **>(param);
			if (!*out && strcmp(obs_source_get_unversioned_id(filter), kHideFilterId) == 0) {
				*out = obs_source_get_ref(filter);
			}
		},
		&found);
	return found;
}

bool HasHideFilter(obs_source_t *source)
{
	OBSSourceAutoRelease filter = GetHideFilter(source);
	return filter.Get() != nullptr;
}

void SetHidden(obs_source_t *source, bool hidden)
{
	OBSSourceAutoRelease filter = GetHideFilter(source);
	if (hidden && !filter) {
		OBSSourceAutoRelease added =
			obs_source_create(kHideFilterId, obs_module_text("GazCast.Clean.FilterName"), nullptr, nullptr);
		if (added) {
			obs_source_filter_add(source, added);
		}
	} else if (!hidden && filter) {
		obs_source_filter_remove(source, filter);
	}
}

struct SourceRow {
	std::string name;
	bool scene;
	bool gazcast;
	bool overlay;
	bool hidden;

	bool operator==(const SourceRow &other) const
	{
		return name == other.name && scene == other.scene && gazcast == other.gazcast &&
		       overlay == other.overlay && hidden == other.hidden;
	}
};

/* Every scene, then every input with video (overlays first). Unticking a scene
 * leaves out everything in it: the filter skips the whole scene, whether it is
 * nested in another scene or is the program scene itself. Nothing is ticked or
 * hidden automatically; the user chooses. */
std::vector<SourceRow> ScanSources()
{
	/* Collect first, then read each source outside obs_enum_sources. */
	std::vector<OBSSource> sources;
	obs_enum_sources(
		[](void *param, obs_source_t *source) {
			if (obs_source_get_output_flags(source) & OBS_SOURCE_VIDEO) {
				static_cast<std::vector<OBSSource> *>(param)->emplace_back(source);
			}
			return true;
		},
		&sources);
	std::vector<OBSSource> scenes;
	obs_enum_scenes(
		[](void *param, obs_source_t *source) {
			static_cast<std::vector<OBSSource> *>(param)->emplace_back(source);
			return true;
		},
		&scenes);

	std::vector<SourceRow> rows;
	for (obs_source_t *scene : scenes) {
		const char *name = obs_source_get_name(scene);
		rows.push_back({name ? name : "", true, false, false, HasHideFilter(scene)});
	}
	for (obs_source_t *source : sources) {
		const bool gazcast = IsGazCastOverlay(source);
		const bool overlay = gazcast || IsOverlay(source);

		const char *name = obs_source_get_name(source);
		rows.push_back({name ? name : "", false, gazcast, overlay, HasHideFilter(source)});
	}

	std::stable_sort(rows.begin(), rows.end(), [](const SourceRow &a, const SourceRow &b) {
		if (a.scene != b.scene) {
			return a.scene;
		}
		if (a.overlay != b.overlay) {
			return a.overlay;
		}
		return QString::fromStdString(a.name).compare(QString::fromStdString(b.name), Qt::CaseInsensitive) < 0;
	});
	return rows;
}

/* ------------------------------------------------------------------------- */
/* Settings: <OBS config>/plugin_config/gazcast-docks/clean-record.json */

bool LoadWithRecording()
{
	BPtr<char> path = obs_module_config_path(kConfigFile);
	OBSDataAutoRelease data = path ? obs_data_create_from_json_file_safe(path, "bak") : nullptr;
	if (!data) {
		return true;
	}
	obs_data_set_default_bool(data, "with_recording", true);
	return obs_data_get_bool(data, "with_recording");
}

void SaveWithRecording(bool on)
{
	BPtr<char> dir = obs_module_config_path("");
	BPtr<char> path = obs_module_config_path(kConfigFile);
	if (!dir || !path) {
		return;
	}
	os_mkdirs(dir);

	OBSDataAutoRelease data = obs_data_create();
	obs_data_set_bool(data, "with_recording", on);
	if (!obs_data_save_json_safe(data, path, "tmp", "bak")) {
		obs_log(LOG_WARNING, "could not write %s", path.Get());
	}
}

/* ------------------------------------------------------------------------- */
/* Recording */

struct Recorder {
	obs_source_t *mix = nullptr;
	obs_view_t *view = nullptr;
	obs_encoder_t *video = nullptr;
	obs_encoder_t *audio = nullptr;
	obs_output_t *output = nullptr;
	bool withMain = false;
	bool stopping = false;
	QString path;
	QElapsedTimer clock;
};

Recorder g_rec;
QString g_lastMessage;
void UpdateDock();

bool EncoderAvailable(const char *id)
{
	const char *type = nullptr;
	for (size_t i = 0; obs_enum_encoder_types(i, &type); i++) {
		if (strcmp(type, id) == 0) {
			return true;
		}
	}
	return false;
}

obs_encoder_t *CreateVideoEncoder(obs_output_t *main)
{
	/* Same encoder and settings as the normal recording, when OBS has one
	 * set up. Not a new reference. */
	obs_encoder_t *mainEncoder = main ? obs_output_get_video_encoder(main) : nullptr;
	if (mainEncoder) {
		OBSDataAutoRelease settings = obs_encoder_get_settings(mainEncoder);
		obs_encoder_t *encoder = obs_video_encoder_create(obs_encoder_get_id(mainEncoder),
								  "gazcast-clean-video", settings, nullptr);
		if (encoder) {
			return encoder;
		}
	}

	/* Otherwise the first hardware encoder this PC has, then x264. */
	static const char *const kFallbacks[] = {"obs_nvenc_h264_tex", "jim_nvenc", "h264_texture_amf",
						 "obs_qsv11_v2",       "obs_qsv11", "obs_x264"};
	for (const char *id : kFallbacks) {
		if (!EncoderAvailable(id)) {
			continue;
		}
		OBSDataAutoRelease settings = obs_data_create();
		obs_data_set_string(settings, "rate_control", "CBR");
		obs_data_set_int(settings, "bitrate", 12000);
		obs_encoder_t *encoder = obs_video_encoder_create(id, "gazcast-clean-video", settings, nullptr);
		if (encoder) {
			return encoder;
		}
	}
	return nullptr;
}

/* The normal recording's first audio track (Advanced output can record other
 * tracks than track 1). Read from the profile so this works on OBS 30. */
size_t RecordingMixer()
{
	config_t *config = obs_frontend_get_profile_config();
	const char *mode = config ? config_get_string(config, "Output", "Mode") : nullptr;
	if (!mode || astrcmpi(mode, "Advanced") != 0) {
		return 0;
	}
	const long long tracks = config_get_int(config, "AdvOut", "RecTracks");
	for (size_t i = 0; i < MAX_AUDIO_MIXES; i++) {
		if (tracks & (1LL << i)) {
			return i;
		}
	}
	return 0;
}

obs_encoder_t *CreateAudioEncoder(obs_output_t *main)
{
	const size_t mixer = RecordingMixer();

	obs_encoder_t *mainEncoder = main ? obs_output_get_audio_encoder(main, 0) : nullptr;
	if (mainEncoder) {
		OBSDataAutoRelease settings = obs_encoder_get_settings(mainEncoder);
		obs_encoder_t *encoder = obs_audio_encoder_create(obs_encoder_get_id(mainEncoder),
								  "gazcast-clean-audio", settings, mixer, nullptr);
		if (encoder) {
			return encoder;
		}
	}

	for (const char *id : {"ffmpeg_aac", "CoreAudio_AAC"}) {
		if (!EncoderAvailable(id)) {
			continue;
		}
		OBSDataAutoRelease settings = obs_data_create();
		obs_data_set_int(settings, "bitrate", 192);
		obs_encoder_t *encoder = obs_audio_encoder_create(id, "gazcast-clean-audio", settings, mixer, nullptr);
		if (encoder) {
			return encoder;
		}
	}
	return nullptr;
}

/* "mp4" when the normal recording uses an MP4 format, otherwise "mkv". */
QString RecordingExtension()
{
	config_t *config = obs_frontend_get_profile_config();
	if (!config) {
		return QStringLiteral("mkv");
	}
	const char *mode = config_get_string(config, "Output", "Mode");
	const char *section = (mode && astrcmpi(mode, "Advanced") == 0) ? "AdvOut" : "SimpleOutput";
	const char *format = config_get_string(config, section, "RecFormat2");
	return (format && strstr(format, "mp4")) ? QStringLiteral("mp4") : QStringLiteral("mkv");
}

/* Next to the normal recording: "<its name> (clean).<ext>" while it runs,
 * otherwise "GazCast clean <date> <time>.<ext>" in the recording folder. */
QString CleanFilePath(obs_output_t *main, const QString &ext)
{
	if (main && obs_output_active(main)) {
		OBSDataAutoRelease settings = obs_output_get_settings(main);
		QString mainPath = QString::fromUtf8(obs_data_get_string(settings, "path"));
		if (mainPath.isEmpty()) {
			mainPath = QString::fromUtf8(obs_data_get_string(settings, "url"));
		}
		const QFileInfo info(mainPath);
		if (!mainPath.isEmpty() && !mainPath.contains(QLatin1String("://")) && info.dir().exists()) {
			return info.dir().filePath(info.completeBaseName() + QStringLiteral(" (clean).") + ext);
		}
	}

	BPtr<char> folder = obs_frontend_get_current_record_output_path();
	QDir dir((folder && *folder) ? QString::fromUtf8(folder.Get()) : QDir::homePath());
	const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd hh-mm-ss"));
	return dir.filePath(QStringLiteral("GazCast clean ") + stamp + QStringLiteral(".") + ext);
}

void OnOutputStop(void *, calldata_t *data);

void ReleaseRecorder()
{
	if (g_rec.output) {
		signal_handler_disconnect(obs_output_get_signal_handler(g_rec.output), "stop", OnOutputStop, nullptr);
		obs_output_release(g_rec.output);
	}
	obs_encoder_release(g_rec.video);
	obs_encoder_release(g_rec.audio);
	if (g_rec.view) {
		obs_view_remove(g_rec.view);
		obs_view_set_source(g_rec.view, 0, nullptr);
		obs_view_destroy(g_rec.view);
	}
	obs_source_release(g_rec.mix);
	g_rec = Recorder();
}

/* Called on the output's thread; finish on the UI thread. */
void OnOutputStop(void *, calldata_t *data)
{
	const long long code = calldata_int(data, "code");
	QString error;
	if (code != OBS_OUTPUT_SUCCESS) {
		obs_output_t *output = static_cast<obs_output_t *>(calldata_ptr(data, "output"));
		const char *last = output ? obs_output_get_last_error(output) : nullptr;
		error = last ? QString::fromUtf8(last) : QString::fromUtf8(obs_module_text("GazCast.Clean.Failed"));
	}

	QMetaObject::invokeMethod(
		QCoreApplication::instance(),
		[error]() {
			if (!g_rec.output) {
				return;
			}
			const QString path = g_rec.path;
			ReleaseRecorder();
			if (error.isEmpty()) {
				g_lastMessage = QString::fromUtf8(obs_module_text("GazCast.Clean.Saved")).arg(path);
				obs_log(LOG_INFO, "clean record: saved %s", path.toUtf8().constData());
			} else {
				g_lastMessage = error;
				obs_log(LOG_WARNING, "clean record stopped with an error: %s",
					error.toUtf8().constData());
			}
			UpdateDock();
		},
		Qt::QueuedConnection);
}

bool StartClean(bool withMain)
{
	if (g_rec.output) {
		return true;
	}

	ScanSources();

	auto fail = [](const QString &why) {
		obs_log(LOG_WARNING, "clean record could not start: %s", why.toUtf8().constData());
		ReleaseRecorder();
		g_lastMessage = why;
		UpdateDock();
		return false;
	};

	obs_video_info ovi;
	if (!obs_get_video_info(&ovi)) {
		return fail(QString::fromUtf8(obs_module_text("GazCast.Clean.Failed")));
	}

	g_rec.mix = obs_source_create_private(kMixSourceId, "GazCast clean mix", nullptr);
	g_rec.view = obs_view_create();
	obs_view_set_source(g_rec.view, 0, g_rec.mix);
	video_t *video = obs_view_add2(g_rec.view, &ovi);
	if (!g_rec.mix || !video) {
		return fail(QString::fromUtf8(obs_module_text("GazCast.Clean.Failed")));
	}

	OBSOutputAutoRelease main = obs_frontend_get_recording_output();
	g_rec.video = CreateVideoEncoder(main);
	g_rec.audio = CreateAudioEncoder(main);
	if (!g_rec.video || !g_rec.audio) {
		return fail(QString::fromUtf8(obs_module_text("GazCast.Clean.NoEncoder")));
	}
	obs_encoder_set_video(g_rec.video, video);
	obs_encoder_set_audio(g_rec.audio, obs_get_audio());

	const QString ext = RecordingExtension();
	g_rec.path = QDir::toNativeSeparators(CleanFilePath(main, ext));

	OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_string(settings, "path", g_rec.path.toUtf8().constData());
	if (ext == QLatin1String("mp4")) {
		/* Fragmented, so the file still plays if OBS or the PC crashes. */
		obs_data_set_string(settings, "muxer_settings", "movflags=frag_keyframe+empty_moov+delay_moov");
	}
	g_rec.output = obs_output_create("ffmpeg_muxer", "gazcast-clean-output", settings, nullptr);
	if (!g_rec.output) {
		return fail(QString::fromUtf8(obs_module_text("GazCast.Clean.Failed")));
	}
	obs_output_set_video_encoder(g_rec.output, g_rec.video);
	obs_output_set_audio_encoder(g_rec.output, g_rec.audio, 0);
	signal_handler_connect(obs_output_get_signal_handler(g_rec.output), "stop", OnOutputStop, nullptr);

	if (!obs_output_start(g_rec.output)) {
		const char *last = obs_output_get_last_error(g_rec.output);
		return fail(last ? QString::fromUtf8(last)
				 : QString::fromUtf8(obs_module_text("GazCast.Clean.Failed")));
	}

	g_rec.withMain = withMain;
	g_rec.clock.start();
	g_lastMessage.clear();
	obs_log(LOG_INFO, "clean record: started %s (%s encoder)", g_rec.path.toUtf8().constData(),
		obs_encoder_get_id(g_rec.video));
	UpdateDock();
	return true;
}

void StopClean()
{
	if (!g_rec.output || g_rec.stopping) {
		return;
	}
	g_rec.stopping = true;
	obs_output_stop(g_rec.output);
	UpdateDock();
}

/* ------------------------------------------------------------------------- */
/* Dock */

struct DockUi {
	QPointer<QWidget> root;
	QPointer<QLabel> status;
	QPointer<QPushButton> button;
	QPointer<QCheckBox> withRecording;
	QPointer<QListWidget> list;
	QPointer<QTimer> timer;
	std::vector<SourceRow> shown;
	bool added = false;
};

DockUi g_ui;

QString Elapsed()
{
	const qint64 secs = g_rec.clock.isValid() ? g_rec.clock.elapsed() / 1000 : 0;
	return QStringLiteral("%1:%2:%3")
		.arg(secs / 3600, 2, 10, QLatin1Char('0'))
		.arg((secs / 60) % 60, 2, 10, QLatin1Char('0'))
		.arg(secs % 60, 2, 10, QLatin1Char('0'));
}

void UpdateDock()
{
	if (!g_ui.root) {
		return;
	}

	if (g_rec.output) {
		g_ui.status->setText(QString::fromUtf8(obs_module_text(g_rec.stopping ? "GazCast.Clean.Stopping"
										      : "GazCast.Clean.Recording"))
					     .arg(Elapsed(), g_rec.path));
		g_ui.button->setText(QString::fromUtf8(obs_module_text("GazCast.Clean.Stop")));
		g_ui.button->setEnabled(!g_rec.stopping);
	} else {
		g_ui.status->setText(g_lastMessage.isEmpty() ? QString::fromUtf8(obs_module_text("GazCast.Clean.Idle"))
							     : g_lastMessage);
		g_ui.button->setText(QString::fromUtf8(obs_module_text("GazCast.Clean.Start")));
		g_ui.button->setEnabled(true);
	}
}

void RefreshList()
{
	if (!g_ui.list) {
		return;
	}

	std::vector<SourceRow> rows = ScanSources();
	if (rows == g_ui.shown) {
		return;
	}
	g_ui.shown = rows;

	const QSignalBlocker block(g_ui.list);
	const int scroll = g_ui.list->verticalScrollBar() ? g_ui.list->verticalScrollBar()->value() : 0;
	g_ui.list->clear();
	for (const SourceRow &row : rows) {
		QString label = QString::fromStdString(row.name);
		if (row.scene) {
			label += QStringLiteral("  ") + QString::fromUtf8(obs_module_text("GazCast.Clean.SceneTag"));
		} else if (row.overlay) {
			label += QStringLiteral("  ") +
				 QString::fromUtf8(obs_module_text(row.gazcast ? "GazCast.Clean.GazCastTag"
									       : "GazCast.Clean.OverlayTag"));
		}
		auto *item = new QListWidgetItem(label, g_ui.list);
		item->setData(Qt::UserRole, QString::fromStdString(row.name));
		item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
		item->setCheckState(row.hidden ? Qt::Unchecked : Qt::Checked);
	}
	if (g_ui.list->verticalScrollBar()) {
		g_ui.list->verticalScrollBar()->setValue(scroll);
	}
}

void OnItemChanged(QListWidgetItem *item)
{
	const std::string name = item->data(Qt::UserRole).toString().toStdString();
	OBSSourceAutoRelease source = obs_get_source_by_name(name.c_str());
	if (source) {
		SetHidden(source, item->checkState() != Qt::Checked);
	}
	RefreshList();
}

QWidget *BuildDock()
{
	auto *root = new QWidget();
	root->setObjectName(QStringLiteral("gazcast-clean-container"));
	root->setMinimumSize(220, 200);

	auto *layout = new QVBoxLayout(root);
	layout->setContentsMargins(8, 8, 8, 8);

	auto *status = new QLabel(root);
	status->setWordWrap(true);
	status->setTextInteractionFlags(Qt::TextSelectableByMouse);
	layout->addWidget(status);

	auto *button = new QPushButton(root);
	button->setMinimumHeight(32);
	layout->addWidget(button);

	auto *withRecording = new QCheckBox(QString::fromUtf8(obs_module_text("GazCast.Clean.WithRecording")), root);
	withRecording->setChecked(LoadWithRecording());
	layout->addWidget(withRecording);

	auto *hint = new QLabel(QString::fromUtf8(obs_module_text("GazCast.Clean.LeaveOut")), root);
	hint->setWordWrap(true);
	layout->addWidget(hint);

	auto *list = new QListWidget(root);
	list->setSelectionMode(QAbstractItemView::NoSelection);
	layout->addWidget(list, 1);

	QObject::connect(button, &QPushButton::clicked, root, []() {
		if (g_rec.output) {
			StopClean();
		} else {
			StartClean(false);
		}
	});
	QObject::connect(withRecording, &QCheckBox::toggled, root, [](bool on) { SaveWithRecording(on); });
	QObject::connect(list, &QListWidget::itemChanged, root, OnItemChanged);

	/* Keeps the list (and the automatic tagging of new GazCast overlays) up
	 * to date, and ticks the recording clock. */
	auto *timer = new QTimer(root);
	timer->setInterval(1000);
	int tick = 0;
	QObject::connect(timer, &QTimer::timeout, root, [tick]() mutable {
		if (g_rec.output) {
			UpdateDock();
		}
		if (++tick % 2 == 0) {
			RefreshList();
		}
	});
	timer->start();

	g_ui.root = root;
	g_ui.status = status;
	g_ui.button = button;
	g_ui.withRecording = withRecording;
	g_ui.list = list;
	g_ui.timer = timer;
	return root;
}

} // namespace

/* ------------------------------------------------------------------------- */

void CleanRecordRegisterTypes()
{
	obs_source_info mix = {};
	mix.id = kMixSourceId;
	mix.type = OBS_SOURCE_TYPE_INPUT;
	mix.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_CAP_DISABLED;
	mix.get_name = MixGetName;
	mix.create = MixCreate;
	mix.destroy = MixDestroy;
	mix.get_width = MixGetWidth;
	mix.get_height = MixGetHeight;
	mix.video_render = MixRender;
	obs_register_source(&mix);

	obs_source_info hide = {};
	hide.id = kHideFilterId;
	hide.type = OBS_SOURCE_TYPE_FILTER;
	hide.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB;
	hide.get_name = HideGetName;
	hide.create = HideCreate;
	hide.destroy = HideDestroy;
	hide.video_render = HideRender;
	obs_register_source(&hide);
}

void CleanRecordCreateDock()
{
	QWidget *root = BuildDock();
	g_ui.added = obs_frontend_add_dock_by_id(kDockId, obs_module_text("GazCast.Dock.Clean"), root);
	if (!g_ui.added) {
		obs_log(LOG_WARNING, "dock id '%s' already in use", kDockId);
		delete root;
		g_ui = DockUi();
		return;
	}
	UpdateDock();
}

void CleanRecordFrontendEvent(enum obs_frontend_event event)
{
	switch (event) {
	case OBS_FRONTEND_EVENT_RECORDING_STARTED:
		if (!g_rec.output && LoadWithRecording()) {
			StartClean(true);
		}
		break;
	case OBS_FRONTEND_EVENT_RECORDING_STOPPING:
		if (g_rec.output && g_rec.withMain) {
			StopClean();
		}
		break;
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
		g_ui.shown.clear();
		RefreshList();
		break;
	default:
		break;
	}
}

void CleanRecordShutdown()
{
	if (g_ui.timer) {
		g_ui.timer->stop();
	}
	if (g_rec.output) {
		/* OBS is closing: stop now instead of waiting for the muxer. */
		signal_handler_disconnect(obs_output_get_signal_handler(g_rec.output), "stop", OnOutputStop, nullptr);
		obs_output_force_stop(g_rec.output);
	}
	ReleaseRecorder();

	if (g_ui.added) {
		obs_frontend_remove_dock(kDockId);
	}
	g_ui = DockUi();
}
