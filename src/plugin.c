/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Screen Glide: one OBS source that shows whichever monitor you are working
 * on, sliding between monitor-capture inputs (no scene switching).
 *
 * The active monitor comes from a state file written by the companion GNOME
 * Shell extension (Wayland gives clients no pointer or focus information).
 * Slide direction follows each screen's physical position, which can differ
 * from the desktop monitor order.
 */
#include <obs-module.h>
#include <graphics/graphics.h>
#include <graphics/vec4.h>
#include <util/platform.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

OBS_DECLARE_MODULE()

const char *obs_module_description(void)
{
	return "Slides between monitor captures following the active monitor";
}

#define SOURCE_ID "screen_glide_source"
#define MAX_SCREENS 4
#define MAX_MONITORS 8
#define POLL_SECONDS 0.05f
#define RESOLVE_SECONDS 1.0f
/* How far the outgoing screen travels, as a fraction of the canvas width. */
#define PARALLAX 0.35f

struct geo {
	int x, y, w, h;
	bool valid;
};

struct sg {
	obs_source_t *self;
	gs_texrender_t *tr;
	gs_texrender_t *tr_a, *tr_b;
	gs_effect_t *fx;
	int style;

	uint32_t width, height;
	int debounce_ms, anim_ms, force;
	bool follow_focus;

	char *name[MAX_SCREENS];
	int monitor[MAX_SCREENS];
	int pos[MAX_SCREENS];
	obs_source_t *child[MAX_SCREENS];

	int cur, prev;
	int cand;
	float cand_time;
	bool animating;
	float anim_t;

	float poll_acc, resolve_acc;
	int pointer_mon, focus_mon;
	struct geo geo[MAX_MONITORS];
};

static const char *anime_effect =
	"uniform float4x4 ViewProj;\n"
	"uniform texture2d tex_a;\n"
	"uniform texture2d tex_b;\n"
	"uniform float progress;\n"
	"uniform float direction;\n"
	"uniform float blur_len;\n"
	"uniform float envelope;\n"
	"sampler_state samp { Filter = Linear; AddressU = Clamp; AddressV = Clamp; };\n"
	"struct VertData { float4 pos : POSITION; float2 uv : TEXCOORD0; };\n"
	"VertData VSDefault(VertData v_in) {\n"
	"  VertData o; o.pos = mul(float4(v_in.pos.xyz, 1.0), ViewProj); o.uv = v_in.uv; return o; }\n"
	"float hash(float n) { return frac(sin(n * 127.1) * 43758.5453); }\n"
	"float tri(float t) { return abs(frac(t) - 0.5) * 2.0; }\n"
	/* Slanted, zig-zag (triangle-toothed) boundary the incoming screen sweeps in with. */
	"float edge_at(float y) {\n"
	"  float base = lerp(1.3, -0.3, progress);\n"
	"  return base + 0.30 * (y - 0.5) + 0.07 * envelope * (tri(y * 6.0) - 0.5);\n"
	"}\n"
	"float4 comp(float2 uv) {\n"
	"  float zoom = 0.045 * envelope;\n"
	"  float2 q = (uv - 0.5) / (1.0 + zoom) + 0.5;\n"
	"  float u = direction > 0.0 ? q.x : 1.0 - q.x;\n"
	"  float m = smoothstep(-0.006, 0.006, u - edge_at(q.y));\n"
	"  float4 a = tex_a.Sample(samp, q);\n"
	"  float4 b = tex_b.Sample(samp, q);\n"
	"  return lerp(a, b, m);\n"
	"}\n"
	"float4 PSAnime(VertData v_in) : TARGET {\n"
	"  float2 uv = v_in.uv;\n"
	"  float4 acc = float4(0.0, 0.0, 0.0, 0.0);\n"
	"  for (int i = 0; i < 14; i++) {\n"
	"    float t = (float(i) / 13.0 - 0.5) * blur_len;\n"
	"    acc += comp(uv + float2(t, 0.0));\n"
	"  }\n"
	"  acc /= 14.0;\n"
	/* Speed lines: long thin wedges along the travel axis, densest around the moving edge. */
	"  float u = direction > 0.0 ? uv.x : 1.0 - uv.x;\n"
	"  float y2 = uv.y + 0.16 * (u - 0.5);\n"
	"  float rows = 48.0;\n"
	"  float id = floor(y2 * rows);\n"
	"  float f = frac(y2 * rows) - 0.5;\n"
	"  float h1 = hash(id);\n"
	"  float h2 = hash(id + 17.0);\n"
	"  float h3 = hash(id + 41.0);\n"
	"  float centre = edge_at(uv.y) + (h2 - 0.5) * 0.8;\n"
	"  float len = 0.2 + 0.5 * h3;\n"
	"  float taper = saturate(1.0 - abs(u - centre) / len);\n"
	"  float hw = (0.05 + 0.40 * h1) * taper + 0.001;\n"
	"  float line = (1.0 - smoothstep(hw * 0.25, hw, abs(f))) * step(0.4, h3);\n"
	"  line *= envelope;\n"
	"  acc.rgb = lerp(acc.rgb, float3(1.0, 1.0, 1.0), saturate(line * 0.9));\n"
	"  acc.a = 1.0;\n"
	"  return acc;\n"
	"}\n"
	"technique Draw {\n"
	"  pass { vertex_shader = VSDefault(v_in); pixel_shader = PSAnime(v_in); }\n"
	"}\n";

static const char *get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "Screen Glide (active monitor)";
}

static void state_path(char *out, size_t len, bool flatpak)
{
	const char *over = getenv("SCREEN_GLIDE_STATE");
	const char *rt = getenv("XDG_RUNTIME_DIR");
	if (over && *over)
		snprintf(out, len, "%s", over);
	else if (flatpak)
		snprintf(out, len, "%s/app/com.obsproject.Studio/screen-glide.state", rt ? rt : "/run/user/1000");
	else
		snprintf(out, len, "%s/screen-glide.state", rt ? rt : "/run/user/1000");
}

static void read_state(struct sg *s)
{
	char path[512], line[160];
	FILE *f = NULL;
	for (int i = 0; i < 2 && !f; i++) {
		state_path(path, sizeof(path), i == 0);
		f = fopen(path, "r");
	}
	if (!f)
		return;
	struct geo geo[MAX_MONITORS];
	memset(geo, 0, sizeof(geo));
	int pointer = s->pointer_mon, focus = -1;
	while (fgets(line, sizeof(line), f)) {
		int i, x, y, w, h;
		if (sscanf(line, "pointer %d", &i) == 1)
			pointer = i;
		else if (sscanf(line, "focus %d", &i) == 1)
			focus = i;
		else if (sscanf(line, "mon %d %d %d %d %d", &i, &x, &y, &w, &h) == 5 && i >= 0 && i < MAX_MONITORS)
			geo[i] = (struct geo){x, y, w, h, true};
	}
	fclose(f);
	s->pointer_mon = pointer;
	s->focus_mon = focus;
	memcpy(s->geo, geo, sizeof(geo));
}

static void release_children(struct sg *s)
{
	for (int i = 0; i < MAX_SCREENS; i++) {
		if (s->child[i]) {
			obs_source_remove_active_child(s->self, s->child[i]);
			obs_source_release(s->child[i]);
			s->child[i] = NULL;
		}
	}
}

static void resolve_children(struct sg *s)
{
	for (int i = 0; i < MAX_SCREENS; i++) {
		obs_source_t *c = NULL;
		if (s->name[i] && *s->name[i])
			c = obs_get_source_by_name(s->name[i]);
		if (c == s->child[i]) {
			obs_source_release(c);
			continue;
		}
		if (s->child[i]) {
			obs_source_remove_active_child(s->self, s->child[i]);
			obs_source_release(s->child[i]);
		}
		s->child[i] = c;
		if (c && !obs_source_add_active_child(s->self, c)) {
			obs_source_release(c);
			s->child[i] = NULL;
		}
	}
}

static void *create(obs_data_t *settings, obs_source_t *source)
{
	struct sg *s = bzalloc(sizeof(*s));
	s->self = source;
	s->cand = s->cur = s->prev = -1;
	s->pointer_mon = 0;
	s->focus_mon = -1;
	obs_enter_graphics();
	s->tr = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	s->tr_a = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	s->tr_b = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	char *err = NULL;
	s->fx = gs_effect_create(anime_effect, "screen-glide-anime", &err);
	if (!s->fx)
		blog(LOG_ERROR, "[screen-glide] anime effect failed, using plain swoosh: %s", err ? err : "unknown");
	bfree(err);
	obs_leave_graphics();
	obs_source_update(source, settings);
	return s;
}

static void destroy(void *data)
{
	struct sg *s = data;
	release_children(s);
	for (int i = 0; i < MAX_SCREENS; i++)
		bfree(s->name[i]);
	obs_enter_graphics();
	gs_texrender_destroy(s->tr);
	gs_texrender_destroy(s->tr_a);
	gs_texrender_destroy(s->tr_b);
	if (s->fx)
		gs_effect_destroy(s->fx);
	obs_leave_graphics();
	bfree(s);
}

static void get_defaults(obs_data_t *d)
{
	static const int mon[MAX_SCREENS] = {0, 2, 1, 3};
	obs_data_set_default_int(d, "width", 1920);
	obs_data_set_default_int(d, "height", 1080);
	obs_data_set_default_int(d, "debounce_ms", 450);
	obs_data_set_default_int(d, "anim_ms", 700);
	obs_data_set_default_int(d, "style", 1);
	obs_data_set_default_int(d, "follow", 0);
	obs_data_set_default_int(d, "force", 0);
	for (int i = 0; i < MAX_SCREENS; i++) {
		char k[16];
		snprintf(k, sizeof(k), "mon%d", i + 1);
		obs_data_set_default_int(d, k, mon[i]);
		snprintf(k, sizeof(k), "pos%d", i + 1);
		obs_data_set_default_int(d, k, i);
	}
}

static void update(void *data, obs_data_t *d)
{
	struct sg *s = data;
	s->width = (uint32_t)obs_data_get_int(d, "width");
	s->height = (uint32_t)obs_data_get_int(d, "height");
	s->debounce_ms = (int)obs_data_get_int(d, "debounce_ms");
	s->anim_ms = (int)obs_data_get_int(d, "anim_ms");
	s->style = (int)obs_data_get_int(d, "style");
	s->follow_focus = obs_data_get_int(d, "follow") == 0;
	s->force = (int)obs_data_get_int(d, "force");
	for (int i = 0; i < MAX_SCREENS; i++) {
		char k[16];
		snprintf(k, sizeof(k), "src%d", i + 1);
		bfree(s->name[i]);
		s->name[i] = bstrdup(obs_data_get_string(d, k));
		snprintf(k, sizeof(k), "mon%d", i + 1);
		s->monitor[i] = (int)obs_data_get_int(d, k);
		snprintf(k, sizeof(k), "pos%d", i + 1);
		s->pos[i] = (int)obs_data_get_int(d, k);
	}
	for (int i = 0; i < MAX_SCREENS; i++)
		for (int j = i + 1; j < MAX_SCREENS; j++)
			if (s->name[i] && *s->name[i] && strcmp(s->name[i], s->name[j]) == 0)
				blog(LOG_WARNING, "[screen-glide] screens %d and %d use the same capture '%s'", i + 1, j + 1,
				     s->name[i]);
	resolve_children(s);
	if (s->cur < 0 || !s->child[s->cur]) {
		s->cur = -1;
		for (int i = 0; i < MAX_SCREENS; i++)
			if (s->child[i]) {
				s->cur = i;
				break;
			}
	}
	s->animating = false;
	s->prev = -1;
}

static bool add_source_cb(void *param, obs_source_t *src)
{
	obs_property_t *p = param;
	if (strcmp(obs_source_get_id(src), SOURCE_ID) == 0)
		return true;
	if (!(obs_source_get_output_flags(src) & OBS_SOURCE_VIDEO))
		return true;
	const char *n = obs_source_get_name(src);
	obs_property_list_add_string(p, n, n);
	return true;
}

static obs_properties_t *get_properties(void *data)
{
	struct sg *s = data;
	obs_properties_t *pr = obs_properties_create();
	obs_property_t *p;

	obs_properties_add_int(pr, "width", "Output width", 320, 7680, 1);
	obs_properties_add_int(pr, "height", "Output height", 240, 4320, 1);

	p = obs_properties_add_list(pr, "follow", "Follow", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, "Focused window (falls back to mouse)", 0);
	obs_property_list_add_int(p, "Mouse pointer", 1);

	p = obs_properties_add_int_slider(pr, "debounce_ms", "Switch delay (ms)", 0, 3000, 25);
	obs_property_set_long_description(p, "Pointer or focus must stay on a monitor this long before sliding");
	obs_properties_add_int_slider(pr, "anim_ms", "Swipe duration (ms)", 100, 2000, 25);
	p = obs_properties_add_list(pr, "style", "Transition style", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, "Swoosh (soft parallax)", 0);
	obs_property_list_add_int(p, "Anime (speed lines, motion blur)", 1);

	p = obs_properties_add_list(pr, "force", "Override", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, "Automatic", 0);
	for (int i = 0; i < MAX_SCREENS; i++) {
		char t[32];
		snprintf(t, sizeof(t), "Hold screen %d", i + 1);
		obs_property_list_add_int(p, t, i + 1);
	}

	for (int i = 0; i < MAX_SCREENS; i++) {
		char k[16], label[64];
		snprintf(k, sizeof(k), "src%d", i + 1);
		snprintf(label, sizeof(label), "Screen %d capture", i + 1);
		p = obs_properties_add_list(pr, k, label, OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
		obs_property_list_add_string(p, "(none)", "");
		obs_enum_sources(add_source_cb, p);

		snprintf(k, sizeof(k), "mon%d", i + 1);
		snprintf(label, sizeof(label), "  Desktop monitor");
		p = obs_properties_add_list(pr, k, label, OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
		for (int m = 0; m < MAX_MONITORS; m++) {
			struct geo *g = &s->geo[m];
			if (!g->valid && m >= 4)
				continue;
			char t[96];
			if (g->valid)
				snprintf(t, sizeof(t), "Monitor %d (%dx%d at %d,%d)", m, g->w, g->h, g->x, g->y);
			else
				snprintf(t, sizeof(t), "Monitor %d", m);
			obs_property_list_add_int(p, t, m);
		}

		snprintf(k, sizeof(k), "pos%d", i + 1);
		p = obs_properties_add_list(pr, k, "  Physical position (left to right)", OBS_COMBO_TYPE_LIST,
					    OBS_COMBO_FORMAT_INT);
		for (int q = 0; q < MAX_SCREENS; q++) {
			char t[24];
			snprintf(t, sizeof(t), "Position %d", q + 1);
			obs_property_list_add_int(p, t, q);
		}
	}
	return pr;
}

static uint32_t get_width(void *data)
{
	return ((struct sg *)data)->width;
}

static uint32_t get_height(void *data)
{
	return ((struct sg *)data)->height;
}

static int screen_for_monitor(struct sg *s, int mon)
{
	for (int i = 0; i < MAX_SCREENS; i++)
		if (s->child[i] && s->monitor[i] == mon)
			return i;
	return -1;
}

static void begin_slide(struct sg *s, int to)
{
	s->prev = s->cur;
	s->cur = to;
	s->animating = true;
	s->anim_t = 0.0f;
}

static void tick(void *data, float seconds)
{
	struct sg *s = data;

	s->resolve_acc += seconds;
	if (s->resolve_acc >= RESOLVE_SECONDS) {
		s->resolve_acc = 0.0f;
		resolve_children(s);
		if (s->cur >= 0 && !s->child[s->cur])
			s->cur = -1;
	}
	s->poll_acc += seconds;
	if (s->poll_acc >= POLL_SECONDS) {
		s->poll_acc = 0.0f;
		read_state(s);
	}

	if (s->animating) {
		float dur = s->anim_ms > 0 ? s->anim_ms / 1000.0f : 0.001f;
		s->anim_t += seconds / dur;
		if (s->anim_t >= 1.0f) {
			s->animating = false;
			s->prev = -1;
		}
		return;
	}

	int want;
	if (s->force > 0 && s->force <= MAX_SCREENS && s->child[s->force - 1]) {
		want = s->force - 1;
		s->cand_time = (float)s->debounce_ms / 1000.0f;
	} else {
		int mon = (s->follow_focus && s->focus_mon >= 0) ? s->focus_mon : s->pointer_mon;
		want = screen_for_monitor(s, mon);
	}

	if (s->cur < 0) {
		for (int i = 0; i < MAX_SCREENS; i++)
			if (s->child[i]) {
				s->cur = i;
				break;
			}
		return;
	}
	if (want < 0 || want == s->cur) {
		s->cand = -1;
		return;
	}
	if (want != s->cand) {
		s->cand = want;
		s->cand_time = 0.0f;
		return;
	}
	s->cand_time += seconds;
	if (s->cand_time * 1000.0f >= (float)s->debounce_ms) {
		s->cand = -1;
		begin_slide(s, want);
	}
}

/* Starts gently, accelerates, then a long soft landing. */
static float swoosh(float t)
{
	float smooth = t * t * (3.0f - 2.0f * t);
	float out = 1.0f - powf(1.0f - t, 4.0f);
	return 0.35f * smooth + 0.65f * out;
}

static float swoosh_speed(float t)
{
	float u = 1.0f - t;
	return 0.35f * 6.0f * t * u + 0.65f * 4.0f * u * u * u;
}

static void draw_child(struct sg *s, obs_source_t *c, float dx, float scale)
{
	float cw = (float)obs_source_get_width(c);
	float ch = (float)obs_source_get_height(c);
	if (cw < 1.0f || ch < 1.0f)
		return;
	float W = (float)s->width, H = (float)s->height;
	float fit = fminf(W / cw, H / ch) * scale;
	gs_matrix_push();
	gs_matrix_translate3f(dx + (W - cw * fit) * 0.5f, (H - ch * fit) * 0.5f, 0.0f);
	gs_matrix_scale3f(fit, fit, 1.0f);
	obs_source_video_render(c);
	gs_matrix_pop();
}

static void dim(struct sg *s, float amount)
{
	if (amount <= 0.0f)
		return;
	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_eparam_t *color = gs_effect_get_param_by_name(solid, "color");
	struct vec4 c;
	vec4_set(&c, 0.0f, 0.0f, 0.0f, amount);
	gs_effect_set_vec4(color, &c);
	while (gs_effect_loop(solid, "Solid"))
		gs_draw_sprite(NULL, 0, s->width, s->height);
}

static void render_child_to(struct sg *s, gs_texrender_t *tr, obs_source_t *c)
{
	gs_texrender_reset(tr);
	if (!gs_texrender_begin(tr, s->width, s->height))
		return;
	struct vec4 clear;
	vec4_zero(&clear);
	gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
	gs_ortho(0.0f, (float)s->width, 0.0f, (float)s->height, -100.0f, 100.0f);
	gs_blend_state_push();
	gs_blend_function_separate(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	draw_child(s, c, 0.0f, 1.0f);
	gs_blend_state_pop();
	gs_texrender_end(tr);
}

static bool render_anime(struct sg *s)
{
	if (!s->fx || !s->animating || s->prev < 0 || !s->child[s->prev])
		return false;
	render_child_to(s, s->tr_a, s->child[s->prev]);
	render_child_to(s, s->tr_b, s->child[s->cur]);
	gs_texture_t *ta = gs_texrender_get_texture(s->tr_a);
	gs_texture_t *tb = gs_texrender_get_texture(s->tr_b);
	if (!ta || !tb)
		return false;

	float t = fminf(s->anim_t, 1.0f);
	gs_effect_set_texture(gs_effect_get_param_by_name(s->fx, "tex_a"), ta);
	gs_effect_set_texture(gs_effect_get_param_by_name(s->fx, "tex_b"), tb);
	gs_effect_set_float(gs_effect_get_param_by_name(s->fx, "progress"), swoosh(t));
	gs_effect_set_float(gs_effect_get_param_by_name(s->fx, "direction"),
			    s->pos[s->cur] >= s->pos[s->prev] ? 1.0f : -1.0f);
	gs_effect_set_float(gs_effect_get_param_by_name(s->fx, "blur_len"), fminf(0.2f, 0.075f * swoosh_speed(t)));
	gs_effect_set_float(gs_effect_get_param_by_name(s->fx, "envelope"), sinf(3.14159265f * swoosh(t)));

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	while (gs_effect_loop(s->fx, "Draw"))
		gs_draw_sprite(NULL, 0, s->width, s->height);
	gs_blend_state_pop();
	return true;
}

static void render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct sg *s = data;
	if (!s->width || !s->height || s->cur < 0 || !s->child[s->cur])
		return;
	if (s->style == 1 && render_anime(s))
		return;

	gs_texrender_reset(s->tr);
	if (!gs_texrender_begin(s->tr, s->width, s->height))
		return;
	struct vec4 clear;
	vec4_zero(&clear);
	gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
	gs_ortho(0.0f, (float)s->width, 0.0f, (float)s->height, -100.0f, 100.0f);
	gs_blend_state_push();
	gs_blend_function_separate(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

	if (s->animating && s->prev >= 0 && s->child[s->prev]) {
		float p = swoosh(fminf(s->anim_t, 1.0f));
		float W = (float)s->width;
		/* +1: new screen sits to the right, so content travels left. */
		float dir = s->pos[s->cur] >= s->pos[s->prev] ? 1.0f : -1.0f;
		/* Outgoing drifts back slower, shrinks a touch and darkens. */
		draw_child(s, s->child[s->prev], -dir * p * PARALLAX * W, 1.0f - 0.07f * p);
		dim(s, 0.55f * p);
		draw_child(s, s->child[s->cur], dir * (1.0f - p) * W, 1.0f);
	} else {
		draw_child(s, s->child[s->cur], 0.0f, 1.0f);
	}

	gs_blend_state_pop();
	gs_texrender_end(s->tr);

	gs_texture_t *tex = gs_texrender_get_texture(s->tr);
	if (!tex)
		return;
	gs_effect_t *def = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_eparam_t *img = gs_effect_get_param_by_name(def, "image");
	gs_effect_set_texture(img, tex);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	while (gs_effect_loop(def, "Draw"))
		gs_draw_sprite(tex, 0, s->width, s->height);
	gs_blend_state_pop();
}

static void enum_active(void *data, obs_source_enum_proc_t cb, void *param)
{
	struct sg *s = data;
	for (int i = 0; i < MAX_SCREENS; i++)
		if (s->child[i])
			cb(s->self, s->child[i], param);
}

static struct obs_source_info info = {
	.id = SOURCE_ID,
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW,
	.get_name = get_name,
	.create = create,
	.destroy = destroy,
	.update = update,
	.get_defaults = get_defaults,
	.get_properties = get_properties,
	.get_width = get_width,
	.get_height = get_height,
	.video_tick = tick,
	.video_render = render,
	.enum_active_sources = enum_active,
	.enum_all_sources = enum_active,
};

bool obs_module_load(void)
{
	obs_register_source(&info);
	return true;
}
