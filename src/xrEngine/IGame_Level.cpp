#include "stdafx.h"
#include "igame_level.h"
#include "igame_persistent.h"

#include "x_ray.h"
#include "std_classes.h"
#include "customHUD.h"
#include "render.h"
#include "gamefont.h"
#include "xrLevel.h"
#include "CameraManager.h"
#include "xr_object.h"
#include "feel_sound.h"

#include "../xrCore/profiler.h"

//#include "securom_api.h"

ENGINE_API IGame_Level* g_pGameLevel = NULL;
ENGINE_API KtChunk g_kt_chunk;
ENGINE_API KtFrameCost g_kt_frame;

void KtChunk::busy() { busy_time = Device.fTimeGlobal; }

static thread_local bool kt_thread_loading = false;
KtThreadFlag::operator bool() const { return kt_thread_loading; }
KtThreadFlag& KtThreadFlag::operator=(bool on)
{
	kt_thread_loading = on;
	return *this;
}

// Reads -kt_chunk <level>,<x>,<y>,<z> and checks that the level has what is needed.
// The keys do not change while the game runs, and the AI map is read on another thread at the same
// time as the level, so they are read once, under a lock.
static xrCriticalSection kt_setup_lock;
static bool kt_setup_done = false;

static void kt_chunk_setup();

ENGINE_API bool kt_chunk_keys()
{
	xrCriticalSectionGuard guard(kt_setup_lock);
	if (!kt_setup_done) kt_chunk_setup();
	kt_setup_done = true;
	return g_kt_chunk.configured;
}

ENGINE_API bool kt_chunk_wanted()
{
	{
		xrCriticalSectionGuard guard(kt_setup_lock);
		if (!kt_setup_done) kt_chunk_setup();
		kt_setup_done = true;
	}
	if (!g_kt_chunk.configured) return false;
	if (!g_kt_chunk.base[0]) return true;
	string_path level, wanted;
	FS.update_path(level, "$level$", "");
	strlwr(level);
	strconcat(sizeof(wanted), wanted, "\\", g_kt_chunk.base, "\\");
	return 0 != strstr(level, wanted);
}

static void kt_chunk_setup()
{
	g_kt_chunk = KtChunk();
	// The keys of the world: the command line and, with -kt_world <file>, a file of the same keys
	// under the levels' folder. The engine reads the command line into 2048 characters, and the
	// description of four levels with their seams had come to 1200.
	static xr_string keys;
	keys = Core.Params;
	if (LPCSTR k = strstr(Core.Params, "-kt_world "))
	{
		string_path name = "", file;
		sscanf(k + 10, "%259[^ ]", name);
		FS.update_path(file, "$game_levels$", name);
		IReader* R = FS.exist(file) ? FS.r_open(file) : nullptr;
		if (R)
		{
			keys += " ";
			keys.append((LPCSTR)R->pointer(), R->length());
			keys += " ";
			for (size_t c = 0; c < keys.size(); c++)
				if (keys[c] == '\r' || keys[c] == '\n' || keys[c] == '\t') keys[c] = ' ';
			Msg("* [kt chunk] the world's keys: %s, %d characters", file, R->length());
			FS.r_close(R);
		}
		else Msg("! [kt chunk] cannot read the world's keys %s", file);
	}
	LPCSTR const params = keys.c_str();
	for (LPCSTR key = strstr(params, "-kt_chunk "); key; key = strstr(key + 1, "-kt_chunk "))
	{
		KtPiece piece;
		piece.name[0] = 0;
		piece.tri_base = piece.tri_count = 0;
		piece.loaded = piece.wanted = piece.manual = false;
		piece.has_walk = false;
		if (4 != sscanf(key + 10, "%63[^, ],%f,%f,%f", piece.name, &piece.offset.x, &piece.offset.y, &piece.offset.z))
		{
			Msg("! [kt chunk] cannot read the key, expected -kt_chunk <level>,<x>,<y>,<z>");
			continue;
		}
		string_path file;
		FS.update_path(file, "$game_levels$", piece.name);
		xr_strcat(file, "\\level.cform");
		if (!FS.exist(file))
		{
			Msg("! [kt chunk] no such level: %s", file);
			continue;
		}
		{
			// the box of its AI map, if it has one: the header of level.ai
			string_path ai;
			FS.update_path(ai, "$game_levels$", piece.name);
			xr_strcat(ai, "\\level.ai");
			if (FS.exist(ai))
			{
				IReader* R = FS.r_open(ai);
				if (R && R->length() >= 40)
				{
					R->advance(16);
					R->r(&piece.walk, sizeof(Fbox));
					piece.walk.min.add(piece.offset);
					piece.walk.max.add(piece.offset);
					piece.has_walk = true;
				}
				if (R) FS.r_close(R);
			}
		}
		g_kt_chunk.pieces.push_back(piece);
		Msg("* [kt chunk] level %s is added at %.2f, %.2f, %.2f", piece.name, piece.offset.x, piece.offset.y, piece.offset.z);
	}
	if (g_kt_chunk.pieces.empty()) return;
	g_kt_chunk.select(0);
	g_kt_chunk.configured = true;
	if (LPCSTR k = strstr(params, "-kt_chunk_base "))
	{
		sscanf(k + 15, "%63[^ ]", g_kt_chunk.base);
		strlwr(g_kt_chunk.base);
	}

	for (LPCSTR k = strstr(params, "-kt_cut "); k; k = strstr(k + 1, "-kt_cut "))
	{
		Fvector a, b;
		int keep_ground = 0;
		const int read = sscanf(k + 8, "%f,%f,%f,%f,%f,%f,%d", &a.x, &a.y, &a.z, &b.x, &b.y, &b.z, &keep_ground);
		if (read < 6)
		{
			Msg("! [kt chunk] cannot read the key, expected -kt_cut <x0>,<y0>,<z0>,<x1>,<y1>,<z1>[,<keep ground>]");
			continue;
		}
		Fbox box;
		box.invalidate();
		box.modify(a);
		box.modify(b);
		g_kt_chunk.cuts.push_back(box);
		g_kt_chunk.cut_keeps_ground.push_back(u8(read == 7 && keep_ground ? 1 : 0));
		Msg("* [kt chunk] cut: %.2f, %.2f, %.2f - %.2f, %.2f, %.2f%s", box.min.x, box.min.y, box.min.z, box.max.x, box.max.y, box.max.z,
		    read == 7 && keep_ground ? ", the ground kept" : "");
	}
	for (LPCSTR k = strstr(params, "-kt_graph_way "); k; k = strstr(k + 1, "-kt_graph_way "))
	{
		KtChunk::GraphWay W;
		if (3 == sscanf(k + 14, "%u,%u,%f", &W.a, &W.b, &W.length)) g_kt_chunk.graph_ways.push_back(W);
	}
	for (LPCSTR k = strstr(params, "-kt_graph_edge "); k; k = strstr(k + 1, "-kt_graph_edge "))
	{
		KtChunk::GraphEdge G;
		if (3 == sscanf(k + 15, "%u,%u,%f", &G.a, &G.b, &G.length)) g_kt_chunk.graph_edges.push_back(G);
	}
	for (LPCSTR k = strstr(params, "-kt_graph_line "); k; k = strstr(k + 1, "-kt_graph_line "))
	{
		KtChunk::GraphLine L;
		int read = 0;
		if (2 != sscanf(k + 15, "%u,%u%n", &L.a, &L.b, &read)) continue;
		LPCSTR p = k + 15 + read;
		L.length = 0.f;
		for (Fvector v; *p == ','; )
		{
			int used = 0;
			if (3 != sscanf(p, ",%f,%f,%f%n", &v.x, &v.y, &v.z, &used)) break;
			p += used;
			if (!L.points.empty()) L.length += L.points.back().distance_to(v);
			L.points.push_back(v);
		}
		if (L.points.size() < 2) continue;
		Msg("* [kt world] the world graph: edge %u - %u goes along a line of %u points, %.0f m", L.a, L.b, L.points.size(), L.length);
		g_kt_chunk.graph_lines.push_back(L);
	}
	for (LPCSTR k = strstr(params, "-kt_graph_drop "); k; k = strstr(k + 1, "-kt_graph_drop "))
	{
		KtChunk::GraphEdge G;
		G.length = 0.f;
		if (2 == sscanf(k + 15, "%u,%u", &G.a, &G.b)) g_kt_chunk.graph_drops.push_back(G);
	}
	for (LPCSTR k = strstr(params, "-kt_graph_at "); k; k = strstr(k + 1, "-kt_graph_at "))
	{
		KtChunk::GraphAt A;
		if (4 == sscanf(k + 13, "%u,%f,%f,%f", &A.vertex, &A.point.x, &A.point.y, &A.point.z)) g_kt_chunk.graph_ats.push_back(A);
	}
	if (LPCSTR k = strstr(params, "-kt_graph_seams "))
	{
		string256 list = "";
		sscanf(k + 16, "%255[^ ]", list);
		strlwr(list);
		string64 name;
		for (int i = 0, n = _GetItemCount(list); i < n; i++)
			g_kt_chunk.graph_seams.push_back(_GetItem(list, i, name));
	}
	for (LPCSTR k = strstr(params, "-kt_hom_cut "); k; k = strstr(k + 1, "-kt_hom_cut "))
	{
		Fvector a, b;
		if (6 != sscanf(k + 12, "%f,%f,%f,%f,%f,%f", &a.x, &a.y, &a.z, &b.x, &b.y, &b.z))
		{
			Msg("! [kt chunk] cannot read the key, expected -kt_hom_cut <x0>,<y0>,<z0>,<x1>,<y1>,<z1>");
			continue;
		}
		Fbox box;
		box.invalidate();
		box.modify(a);
		box.modify(b);
		g_kt_chunk.hom_cuts.push_back(box);
	}
	for (LPCSTR k = strstr(params, "-kt_open "); k; k = strstr(k + 1, "-kt_open "))
	{
		Fvector a, b;
		if (6 != sscanf(k + 9, "%f,%f,%f,%f,%f,%f", &a.x, &a.y, &a.z, &b.x, &b.y, &b.z)) continue;
		Fbox box;
		box.invalidate();
		box.modify(a);
		box.modify(b);
		g_kt_chunk.opens.push_back(box);
	}
	if (LPCSTR k = strstr(params, "-kt_ground "))
	{
		string256 list = "";
		sscanf(k + 11, "%255[^ ]", list);
		for (int i = 0, n = _GetItemCount(list); i < n; i++)
		{
			string32 item;
			g_kt_chunk.ground_materials.push_back(u32(atoi(_GetItem(list, i, item))));
		}
	}
	g_kt_frame.log_ms = 50.f;
	if (LPCSTR k = strstr(params, "-kt_frame_log ")) sscanf(k + 14, "%f", &g_kt_frame.log_ms);
	if (LPCSTR k = strstr(params, "-kt_spawn_budget ")) sscanf(k + 17, "%f", &g_kt_chunk.spawn_budget);
	if (LPCSTR k = strstr(params, "-kt_online_per_frame ")) sscanf(k + 21, "%u", &g_kt_chunk.online_per_frame);
	Msg("* [kt spawn] at most %u objects go online in a frame%s", g_kt_chunk.online_per_frame, g_kt_chunk.online_per_frame ? "" : " (off: any number)");
	if (LPCSTR k = strstr(params, "-kt_icon "))
	{
		int on = 1;
		sscanf(k + 9, "%d", &on);
		g_kt_chunk.icon = on != 0;
	}
	Msg("* [kt spawn] objects come online after their models by %.0f ms a frame at most%s", g_kt_chunk.spawn_budget, g_kt_chunk.spawn_budget > 0.f ? "" : " (off: all at once)");
	if (g_kt_frame.log_ms > 0.f) Msg("* [kt frame] frames longer than %.0f ms are told to the log by stages", g_kt_frame.log_ms);
	if (LPCSTR k = strstr(params, "-kt_static_radius "))
	{
		sscanf(k + 18, "%f", &g_kt_chunk.static_radius);
		Msg("* [kt world] objects that stay online on the level leave beyond %.0f m", g_kt_chunk.static_radius);
	}
	for (LPCSTR k = strstr(params, "-kt_trim "); k; k = strstr(k + 1, "-kt_trim "))
	{
		KtChunk::Trim T;
		if (6 != sscanf(k + 9, "%u,%f,%f,%f,%f,%f", &T.piece, &T.px, &T.pz, &T.nx, &T.nz, &T.radius))
		{
			Msg("! [kt chunk] cannot read the key, expected -kt_trim <piece>,<px>,<pz>,<nx>,<nz>,<radius>");
			continue;
		}
		g_kt_chunk.trims.push_back(T);
	}
	if (LPCSTR k = strstr(params, "-kt_stream "))
	{
		sscanf(k + 11, "%f,%f", &g_kt_chunk.stream_in, &g_kt_chunk.stream_out);
		if (g_kt_chunk.stream_out < g_kt_chunk.stream_in) g_kt_chunk.stream_out = g_kt_chunk.stream_in;
		Msg("* [kt stream] neighbours are taken in nearer than %.0f m and let go beyond %.0f m", g_kt_chunk.stream_in, g_kt_chunk.stream_out);
	}
	if (LPCSTR k = strstr(params, "-kt_reach "))
	{
		sscanf(k + 10, "%f,%f", &g_kt_chunk.reach_in, &g_kt_chunk.reach_out);
		if (g_kt_chunk.reach_out < g_kt_chunk.reach_in) g_kt_chunk.reach_out = g_kt_chunk.reach_in;
		Msg("* [kt stream] within that, a neighbour is held when it is nearer than %.0f m of walking (let go beyond %.0f m) or is seen", g_kt_chunk.reach_in, g_kt_chunk.reach_out);
	}
	if (LPCSTR k = strstr(params, "-kt_location_depth "))
		sscanf(k + 19, "%f,%f", &g_kt_chunk.location_depth, &g_kt_chunk.location_wait);
	Msg("* [kt location] scripts hear of another location when the actor is %.0f m of walking from the one he left or has stayed %.0f s",
	    g_kt_chunk.location_depth, g_kt_chunk.location_wait);
	if (LPCSTR k = strstr(params, "-kt_sight "))
	{
		string_path name = "", file;
		sscanf(k + 10, "%259[^, ],%f,%f", name, &g_kt_chunk.sight_in, &g_kt_chunk.sight_out);
		if (g_kt_chunk.sight_out < g_kt_chunk.sight_in) g_kt_chunk.sight_out = g_kt_chunk.sight_in;
		FS.update_path(file, "$game_levels$", name);
		IReader* R = FS.exist(file) ? FS.r_open(file) : 0;
		if (R && R->length() >= 28 && R->r_u32() == 0x5453544b) // "KTST"
		{
			g_kt_chunk.sight_pieces = R->r_u32();
			g_kt_chunk.sight_x0 = R->r_float();
			g_kt_chunk.sight_z0 = R->r_float();
			g_kt_chunk.sight_cell = R->r_float();
			g_kt_chunk.sight_nx = R->r_u32();
			g_kt_chunk.sight_nz = R->r_u32();
			u32 size = g_kt_chunk.sight_nx * g_kt_chunk.sight_nz * g_kt_chunk.sight_pieces;
			if (size && u32(R->elapsed()) >= size * sizeof(u16))
			{
				g_kt_chunk.sight.resize(size);
				R->r(&g_kt_chunk.sight[0], size * sizeof(u16));
				Msg("* [kt stream] sight: %u x %u cells of %.1f m from %.1f, %.1f for %u levels; a neighbour seen nearer than %.0f m is taken, let go when nothing of it is seen within %.0f m",
				    g_kt_chunk.sight_nx, g_kt_chunk.sight_nz, g_kt_chunk.sight_cell, g_kt_chunk.sight_x0, g_kt_chunk.sight_z0, g_kt_chunk.sight_pieces, g_kt_chunk.sight_in, g_kt_chunk.sight_out);
			}
		}
		if (g_kt_chunk.sight.empty()) Msg("! [kt stream] cannot read the table of sight %s", file);
		if (R) FS.r_close(R);
	}
	if (LPCSTR k = strstr(params, "-kt_fake "))
		sscanf(k + 9, "%u", &g_kt_chunk.fake_material);
	if (!g_kt_chunk.opens.empty())
		Msg("* [kt chunk] %u boxes where walls of material %u are taken away", u32(g_kt_chunk.opens.size()), g_kt_chunk.fake_material);
	if (LPCSTR k = strstr(params, "-kt_mask "))
	{
		string_path name = "", file;
		sscanf(k + 9, "%259[^ ]", name);
		FS.update_path(file, "$game_levels$", name);
		IReader* R = FS.exist(file) ? FS.r_open(file) : 0;
		if (R && R->length() >= 28 && R->r_u32() == 0x4b4d544b) // "KTMK"
		{
			g_kt_chunk.mask_pieces = R->r_u32();
			g_kt_chunk.mask_x0 = R->r_float();
			g_kt_chunk.mask_z0 = R->r_float();
			g_kt_chunk.mask_cell = R->r_float();
			g_kt_chunk.mask_nx = R->r_u32();
			g_kt_chunk.mask_nz = R->r_u32();
			u32 size = g_kt_chunk.mask_nx * g_kt_chunk.mask_nz;
			if (u32(R->elapsed()) >= size)
			{
				g_kt_chunk.mask.resize(size);
				R->r(&g_kt_chunk.mask[0], size);
				Msg("* [kt chunk] mask: %u x %u cells of %.1f m from %.1f, %.1f for %u pieces", g_kt_chunk.mask_nx, g_kt_chunk.mask_nz,
				    g_kt_chunk.mask_cell, g_kt_chunk.mask_x0, g_kt_chunk.mask_z0, g_kt_chunk.mask_pieces);
			}
		}
		if (g_kt_chunk.mask.empty()) Msg("! [kt chunk] cannot read the mask %s", file);
		if (R) FS.r_close(R);
	}
	if (LPCSTR k = strstr(params, "-kt_land "))
	{
		string_path name = "", file;
		sscanf(k + 9, "%259[^ ]", name);
		FS.update_path(file, "$game_levels$", name);
		IReader* R = FS.exist(file) ? FS.r_open(file) : 0;
		const u32 size = g_kt_chunk.mask_nx * g_kt_chunk.mask_nz;
		if (R && size && R->length() >= 12 && R->r_u32() == 0x594c544b && R->r_u32() == g_kt_chunk.mask_nx && R->r_u32() == g_kt_chunk.mask_nz &&
			u32(R->elapsed()) >= size * 2) // "KTLY"
		{
			g_kt_chunk.land_y.resize(size);
			R->r(&g_kt_chunk.land_y[0], size * 2);
			if (u32(R->elapsed()) >= size * 2)
			{
				g_kt_chunk.land_shift.resize(size);
				R->r(&g_kt_chunk.land_shift[0], size * 2);
			}
			Msg("* [kt chunk] land: the heights of the ground of our own are read, %s", name);
		}
		else
			Msg("! [kt chunk] cannot read the land's heights %s", file);
		if (R) FS.r_close(R);
	}
	for (LPCSTR k = strstr(params, "-kt_ai_piece "); k; k = strstr(k + 1, "-kt_ai_piece "))
	{
		string_path piece = "";
		if (1 != sscanf(k + 13, "%259[^ ]", piece)) continue;
		g_kt_chunk.ai_pieces.push_back(piece);
		Msg("* [kt chunk] AI piece: %s", piece);
	}
}
extern BOOL g_bLoaded;

IGame_Level::IGame_Level()
{
	PROF_EVENT("IGame_Level::IGame_Level");
	m_pCameras = xr_new<CCameraManager>(true);
	g_pGameLevel = this;
	pLevel = NULL;
	bReady = false;
	pCurrentEntity = NULL;
	pCurrentViewEntity = NULL;
	Device.DumpResourcesMemoryUsage();
}

//#include "resourcemanager.h"

static CDB::MODEL* kt_fresh;
static xr_atomic_u32 kt_stream_state;
static xr_task_group kt_stream_task;

IGame_Level::~IGame_Level()
{
	Device.secondary_tasks.wait();
	// seamless world experiment (kt): a collision model being built aside
	kt_stream_task.wait();
	xr_delete(kt_fresh);
	kt_stream_state = 0;

	if (Core.ParamsData.test(ECoreParams::nes_texture_storing))
		Device.m_pRender->ResourcesStoreNecessaryTextures();
	xr_delete(pLevel);

	// Render-level unload
	Render->level_Unload();
	xr_delete(m_pCameras);
	// Unregister
	Device.seqParallel.clear_not_free();
	Device.seqRender.Remove(this);
	Device.seqFrame.Remove(this);
	CCameraManager::ResetPP();
	///////////////////////////////////////////
	Sound->set_geometry_occ(NULL);
	Sound->set_handler(NULL);
	Device.DumpResourcesMemoryUsage();

	u32 m_base = 0, c_base = 0, m_lmaps = 0, c_lmaps = 0;
	if (Device.m_pRender)
		Device.m_pRender->ResourcesGetMemoryUsage(m_base, c_base, m_lmaps, c_lmaps);

	Msg("* [ D3D ]: textures[%d K]", (m_base + m_lmaps) / 1024);
}

void IGame_Level::net_Stop()
{
    for (int i = 0; i < 6; i++)
    {
        Objects.Update(false);
        Objects.ProcessDestroyQueue();
    }
		
	// Destroy all objects
	Objects.Unload();
	IR_Release();

	bReady = false;
}

//-------------------------------------------------------------------------------------------
//extern CStatTimer tscreate;
void __stdcall _sound_event(ref_sound_data_ptr S, float range)
{
	if (g_pGameLevel && S && S->feedback) g_pGameLevel->SoundEvent_Register(S, range);
}

static void __stdcall build_callback(Fvector* V, int Vcnt, CDB::TRI* T, int Tcnt, void* params)
{
	g_pGameLevel->Load_GameSpecific_CFORM(T, Tcnt);
}

xrCriticalSection lloadcs;
// Seamless world experiment (kt): what the collision loader asks for every triangle and when done.
static void kt_cform_built(Fvector* V, int Vcnt, CDB::TRI* T, int Tcnt, void* params)
{
	g_pGameLevel->Load_GameSpecific_CFORM(T, Tcnt);
}

static bool kt_cform_cut(u32 piece, u32 material, const Fvector& a, const Fvector& b, const Fvector& c)
{
	if (g_kt_chunk.opened(material, a, b, c)) return true;
	// a big triangle is far scenery or a screen: it must not hang over the neighbour
	const float big = 12.f * 12.f;
	const bool strict = a.distance_to_sqr(b) > big || b.distance_to_sqr(c) > big || c.distance_to_sqr(a) > big;
	// ground: by material, and only what lies flat enough to be walked on
	Fvector n;
	n.mknormal(a, b, c);
	const bool is_ground = _abs(n.y) > .5f && g_kt_chunk.ground(material);
	return g_kt_chunk.cut(piece, a, b, c, strict, is_ground);
}

// Pieces taken in and let go on the move. The static collision model is one for the level, so a
// new one is built aside for the wanted set of pieces (a worker thread: reading, the tree), and on
// the main thread between frames the models change places and the renderer drops or reads the
// pieces' geometry. The AI map, the world graph and the objects are not touched: they are the
// part of the world that is always there.
// kt_fresh, kt_stream_state (0 idle, 1 building, 2 built) and kt_stream_task stand above, by the destructor
static xr_vector<KtCform> kt_stream_job;
static bool kt_stream_job_base = true;

static float kt_box_distance(const Fbox& box, const Fvector& C)
{
	const float dx = _max(_max(box.min.x - C.x, C.x - box.max.x), 0.f);
	const float dz = _max(_max(box.min.z - C.z, C.z - box.max.z), 0.f);
	return _sqrt(dx * dx + dz * dz);
}
static CTimer kt_stream_timer;

// What the rule says about a walkable piece: +1 take it in, -1 let it go, 0 leave it as it is.
// piece: 0 the level, 1.. the attached ones. The distance to the box is the outer bound, as it was;
// within it, with -kt_reach, the piece is wanted only when the way there is short or it is seen.
static int kt_stream_rule(u32 piece, const Fbox& walk, bool loaded, const xr_vector<float>* reach)
{
	const Fvector& C = Device.vCameraPosition;
	const float distance = kt_box_distance(walk, C);
	if (distance > g_kt_chunk.stream_out) return loaded ? -1 : 0;
	const u16 seen = g_kt_chunk.sight.empty() ? u16(KtChunk::sight_none) : g_kt_chunk.sight_at(piece, C.x, C.z);
	if (g_kt_chunk.reach_in <= 0.f || !reach || piece >= reach->size() || seen == KtChunk::sight_unknown)
		return (!loaded && distance < g_kt_chunk.stream_in) ? 1 : 0;
	const float way = (*reach)[piece];
	const float sight = seen == KtChunk::sight_none ? flt_max : float(seen);
	if (loaded) return (way > g_kt_chunk.reach_out && sight > g_kt_chunk.sight_out) ? -1 : 0;
	return (way < g_kt_chunk.reach_in || sight < g_kt_chunk.sight_in) ? 1 : 0;
}

void IGame_Level::kt_stream_tick()
{
	if (!g_kt_chunk.active || !bReady) return;
	xr_vector<KtPiece>& pieces = g_kt_chunk.pieces;

	if (kt_stream_state == 0)
	{
		bool change = false;
		static xr_vector<float> reach;
		static float reach_lost = 0.f; // seconds the walking distances have not been known
		const bool reach_asked = g_kt_chunk.reach_in > 0.f && g_kt_chunk.stream_in > 0.f;
		const bool reach_known = reach_asked && kt_reach(reach);
		if (reach_known) reach_lost = 0.f;
		else if (reach_asked)
		{
			// The actor was just put elsewhere and its node is still the old one: for a moment nothing
			// is known, and the distance alone would take in every level around for nothing. Only when
			// it lasts (the free camera, the actor off the AI map) the distance decides.
			reach_lost += Device.fTimeDelta;
			if (reach_lost < 3.f) return;
		}
		for (u32 i = 0; i < pieces.size(); i++)
		{
			KtPiece& P = pieces[i];
			if (!P.manual && P.has_walk && g_kt_chunk.stream_in > 0.f)
			{
				const int rule = kt_stream_rule(i + 1, P.walk, P.loaded, reach_known ? &reach : 0);
				if (rule < 0) P.wanted = false;
				else if (rule > 0) P.wanted = true;
			}
			if (P.wanted != P.loaded) change = true;
		}
		if (!g_kt_chunk.base_manual && g_kt_chunk.base_has_walk && g_kt_chunk.stream_in > 0.f)
		{
			const int rule = kt_stream_rule(0, g_kt_chunk.base_walk, g_kt_chunk.base_loaded, reach_known ? &reach : 0);
			if (rule < 0) g_kt_chunk.base_wanted = false;
			else if (rule > 0) g_kt_chunk.base_wanted = true;
		}
		// something walkable must stay
		bool any = g_kt_chunk.base_wanted;
		for (u32 i = 0; i < pieces.size(); i++)
			if (pieces[i].wanted && pieces[i].has_walk) any = true;
		if (!any) g_kt_chunk.base_wanted = true;
		if (g_kt_chunk.base_wanted != g_kt_chunk.base_loaded) change = true;
		if (!change) return;

		kt_stream_job.clear();
		for (u32 i = 0; i < pieces.size(); i++)
		{
			if (!pieces[i].wanted) continue;
			KtCform form;
			FS.update_path(form.file, "$game_levels$", pieces[i].name);
			xr_strcat(form.file, "\\level.cform");
			form.offset = pieces[i].offset;
			form.piece = i;
			form.tri_base = form.tri_count = 0;
			kt_stream_job.push_back(form);
		}
		kt_stream_job_base = g_kt_chunk.base_wanted;
		kt_fresh = xr_new<CDB::MODEL>();
		kt_stream_state = 1;
		kt_stream_timer.Start();
		kt_stream_task.run([this]()
		{
			ObjectSpace.kt_BuildModel(*kt_fresh, kt_stream_job_base, kt_cform_built, kt_stream_job, kt_cform_cut);
			// the geometry buffers of the pieces on their way in are read and made here, the main thread
			// only puts them in place; the level file it still reads (visuals, sectors) into the system's cache
			for (u32 n = 0; n < kt_stream_job.size(); n++)
			{
				const KtPiece& P = g_kt_chunk.pieces[kt_stream_job[n].piece];
				if (P.loaded) continue;
				Render->kt_piece_stage(kt_stream_job[n].piece);
				static const LPCSTR files[1] = {"\\level"};
				for (u32 f = 0; f < 1; f++)
				{
					string_path file;
					FS.update_path(file, "$game_levels$", P.name);
					xr_strcat(file, files[f]);
					if (!FS.exist(file)) continue;
					IReader* R = FS.r_open(file);
					if (!R) continue;
					const u8* data = (const u8*)R->pointer();
					volatile u32 sum = 0;
					for (int at = 0, size = R->length(); at < size; at += 4096) sum += data[at];
					FS.r_close(R);
				}
			}
			if (kt_stream_job_base && !g_kt_chunk.base_loaded) Render->kt_piece_stage(u32(-1));
			kt_stream_state = 2;
		});
		return;
	}
	g_kt_chunk.busy(); // the icon in the corner while a neighbour is prepared
	// the materials of the pieces on their way in are made a few per frame meanwhile
	bool prepared = true;
	for (u32 n = 0; n < kt_stream_job.size(); n++)
		if (!pieces[kt_stream_job[n].piece].loaded && !Render->kt_piece_prepare(kt_stream_job[n].piece))
		{
			prepared = false;
			break;
		}
	// and the level's own, when it is to come back (it had none made ahead: 0,65 s of it on the main thread)
	if (prepared && kt_stream_job_base && !g_kt_chunk.base_loaded && !Render->kt_piece_prepare(u32(-1))) prepared = false;
	if (kt_stream_state != 2 || !prepared) return;

	kt_stream_task.wait();
	Device.secondary_tasks.wait(); // nobody else is to be inside the model or the geometry now
	const u32 built_ms = kt_stream_timer.GetElapsed_ms();
	CTimer timer;
	timer.Start();
	ObjectSpace.kt_SwapStatic(*kt_fresh);
	const u32 swap_ms = timer.GetElapsed_ms();
	// the model that served until now is freed aside (up to 35 ms of the main thread)
	CDB::MODEL* kt_old = kt_fresh;
	kt_fresh = nullptr;
	kt_stream_task.run([kt_old]() { CDB::MODEL* model = kt_old; xr_delete(model); });
	const u32 freed_ms = timer.GetElapsed_ms();

	xr_vector<u8> stays(pieces.size(), 0);
	for (u32 i = 0; i < pieces.size(); i++) pieces[i].tri_base = pieces[i].tri_count = 0;
	for (u32 n = 0; n < kt_stream_job.size(); n++)
	{
		KtPiece& P = pieces[kt_stream_job[n].piece];
		P.tri_base = kt_stream_job[n].tri_base;
		P.tri_count = kt_stream_job[n].tri_count;
		stays[kt_stream_job[n].piece] = 1;
	}
	string256 gone = "", come = "";
	if (g_kt_chunk.base_loaded && !kt_stream_job_base)
	{
		Render->kt_base_render(false);
		g_kt_chunk.base_loaded = false;
		xr_strcat(gone, " the level itself");
	}
	for (u32 i = 0; i < pieces.size(); i++)
		if (pieces[i].loaded && !stays[i])
		{
			Render->kt_piece_render(i, false);
			pieces[i].loaded = false;
			xr_strcat(gone, " ");
			xr_strcat(gone, pieces[i].name);
		}
	const u32 gone_ms = timer.GetElapsed_ms();
	for (u32 i = 0; i < pieces.size(); i++)
	{
		if (!stays[i]) continue;
		if (pieces[i].loaded)
			Render->kt_piece_sectors(i);
		else
		{
			Render->kt_piece_render(i, true);
			pieces[i].loaded = true;
			xr_strcat(come, " ");
			xr_strcat(come, pieces[i].name);
		}
	}
	if (!g_kt_chunk.base_loaded && kt_stream_job_base)
	{
		Render->kt_base_render(true);
		g_kt_chunk.base_loaded = true;
		xr_strcat(come, " the level itself");
	}
	Render->kt_prepare_reset();
	Render->kt_stage_drop();
	Sound->set_geometry_occ(ObjectSpace.GetStaticModel());
	Msg("* [kt stream] let go:%s; taken in:%s. Collisions built aside in %u ms; on the main thread: swap %u, old model freed %u, geometry dropped %u, geometry read %u ms",
	    gone[0] ? gone : " -", come[0] ? come : " -", built_ms, swap_ms, freed_ms - swap_ms, gone_ms - freed_ms, timer.GetElapsed_ms() - gone_ms);
	kt_stream_state = 0;
}

bool IGame_Level::Load(u32 dwNum)
{
	PROF_EVENT("IGame_Level::Load");
	xrCriticalSectionGuard guard(&lloadcs);
	if (bReady) return TRUE;
	extern xr_task_group prefetch_task;
	prefetch_task.wait();
	//SECUROM_MARKER_PERFORMANCE_ON(10)

	// Initialize level data
	pApp->Level_Set(dwNum);
	string_path temp;
	if (!FS.exist(temp, "$level$", "level.ltx"))
		Debug.fatal(DEBUG_INFO, "Can't find level configuration file '%s'.", temp);
	pLevel = xr_new<CInifile>(temp);

	// Open
	// g_pGamePersistent->LoadTitle ("st_opening_stream");
	g_pGamePersistent->LoadTitle();
	IReader* LL_Stream = FS.r_open("$level$", "level");
	IReader& fs = *LL_Stream;

	// Header
	hdrLEVEL H;
	fs.r_chunk_safe(fsL_HEADER, &H, sizeof(H));
	R_ASSERT2(XRCL_PRODUCTION_VERSION == H.XRLC_version, "Incompatible level version.");

	// CForms
	// g_pGamePersistent->LoadTitle ("st_loading_cform");
	g_pGamePersistent->LoadTitle();
	g_kt_chunk.active = kt_chunk_wanted();
	if (g_kt_chunk.active)
	{
		CTimer kt_timer;
		kt_timer.Start();
		xr_vector<KtCform> cforms(g_kt_chunk.pieces.size());
		for (u32 i = 0; i < cforms.size(); i++)
		{
			FS.update_path(cforms[i].file, "$game_levels$", g_kt_chunk.pieces[i].name);
			xr_strcat(cforms[i].file, "\\level.cform");
			cforms[i].offset = g_kt_chunk.pieces[i].offset;
			cforms[i].piece = i;
			g_kt_chunk.pieces[i].loaded = g_kt_chunk.pieces[i].wanted = true;
			g_kt_chunk.pieces[i].manual = false;
		}
		ObjectSpace.LoadWithChunks(kt_cform_built, cforms, kt_cform_cut);
		g_kt_chunk.base_loaded = g_kt_chunk.base_wanted = true;
		g_kt_chunk.base_manual = g_kt_chunk.base_has_walk = false;
		if (IReader* R = FS.r_open("$level$", "level.ai"))
		{
			if (R->length() >= 40)
			{
				R->advance(16);
				R->r(&g_kt_chunk.base_walk, sizeof(Fbox));
				g_kt_chunk.base_has_walk = true;
			}
			FS.r_close(R);
		}
		for (u32 i = 0; i < cforms.size(); i++)
		{
			g_kt_chunk.pieces[i].tri_base = cforms[i].tri_base;
			g_kt_chunk.pieces[i].tri_count = cforms[i].tri_count;
			Msg("* [kt chunk] collision of %s: triangles %u..%u of the static model", g_kt_chunk.pieces[i].name, cforms[i].tri_base, cforms[i].tri_base + cforms[i].tri_count);
		}
		Msg("* [kt time] collisions of the level and %u pieces: %u ms", u32(cforms.size()), kt_timer.GetElapsed_ms());
	}
	else
	ObjectSpace.Load( [](Fvector* V, int Vcnt, CDB::TRI* T, int Tcnt, void* params){g_pGameLevel->Load_GameSpecific_CFORM(T, Tcnt);});
	//Sound->set_geometry_occ ( &Static );
	Sound->set_geometry_occ(ObjectSpace.GetStaticModel());
	Sound->set_handler(_sound_event);

	pApp->LoadSwitch();


	// HUD + Environment
	if (!g_hud)
		g_hud = (CCustomHUD*)NEW_INSTANCE(CLSID_HUDMANAGER);

	// Render-level Load
	Render->level_Load(LL_Stream);
	// tscreate.FrameEnd ();
	// Msg ("* S-CREATE: %f ms, %d times",tscreate.result,tscreate.count);

	// Objects
	g_pGamePersistent->Environment().mods_load();
	R_ASSERT(Load_GameSpecific_Before());
	Objects.Load();
	//. ANDY R_ASSERT (Load_GameSpecific_After ());

	// Done
	FS.r_close(LL_Stream);
	bReady = true;
	if (!g_dedicated_server) IR_Capture();
#ifndef DEDICATED_SERVER
	Device.seqRender.Add(this);
#endif

	Device.seqFrame.Add(this);

	//SECUROM_MARKER_PERFORMANCE_OFF(10)

	return true;
}

#ifndef _EDITOR
#include "../xrCPU_Pipe/ttapi.h"
#endif

int psNET_DedicatedSleep = 5;

void IGame_Level::OnRender()
{
#ifndef DEDICATED_SERVER
	// if (_abs(Device.fTimeDelta)<EPS_S) return;

#ifdef _GPA_ENABLED
    TAL_ID rtID = TAL_MakeID( 1 , Core.dwFrame , 0);
    TAL_CreateID( rtID );
    TAL_BeginNamedVirtualTaskWithID( "GameRenderFrame" , rtID );
    TAL_Parami( "Frame#" , Device.dwFrame );
    TAL_EndVirtualTask();
#endif // _GPA_ENABLED

	// Level render, only when no client output required
	if (!g_dedicated_server)
	{
		{
			PROF_EVENT("IGame_Level::OnRender: Calculate");
			Render->Calculate();
		}
		{
			PROF_EVENT("IGame_Level::OnRender: Render");
			Render->Render();
		}
	}
	else
	{
		Sleep(psNET_DedicatedSleep);
	}

#ifdef _GPA_ENABLED
    TAL_RetireID( rtID );
#endif // _GPA_ENABLED

	// Font
	// pApp->pFontSystem->SetSizeI(0.023f);
	// pApp->pFontSystem->OnRender ();
#endif
}

void IGame_Level::OnFrame()
{
	PROF_EVENT("IGame_Level::OnFrame");
	{
		KtFrameScope kt_cost(KtFrameCost::stream);
		kt_stream_tick();
	}
	// Log ("- level:on-frame: ",u32(Device.dwFrame));
	// if (_abs(Device.fTimeDelta)<EPS_S) return;

	// Update all objects
	VERIFY(bReady);
	Objects.Update(false);
	g_hud->OnFrame();

	// Ambience
	if (Sounds_Random.size() && (Device.dwTimeGlobal > Sounds_Random_dwNextTime))
	{
		Sounds_Random_dwNextTime = Device.dwTimeGlobal + ::Random.randI(10000, 20000);
		Fvector pos;
		pos.random_dir().normalize().mul(::Random.randF(30, 100)).add(Device.vCameraPosition);
		int id = ::Random.randI(Sounds_Random.size());
		if (Sounds_Random_Enabled)
		{
			Sounds_Random[id].play_at_pos(0, pos, 0);
			Sounds_Random[id].set_volume(1.f);
			Sounds_Random[id].set_range(10, 200);
		}
	}
}

// ==================================================================================================

void CServerInfo::AddItem(LPCSTR name_, LPCSTR value_, u32 color_)
{
	shared_str s_name(name_);
	AddItem(s_name, value_, color_);
}

void CServerInfo::AddItem(shared_str& name_, LPCSTR value_, u32 color_)
{
	SItem_ServerInfo it;
	// shared_str s_name = CStringTable().translate( name_ );

	// xr_strcpy( it.name, s_name.c_str() );
	xr_strcpy(it.name, name_.c_str());
	xr_strcat(it.name, " = ");
	xr_strcat(it.name, value_);
	it.color = color_;

	if (data.size() < max_item)
	{
		data.push_back(it);
	}
}

void IGame_Level::SetEntity(CObject* O)
{
	if (pCurrentEntity)
		pCurrentEntity->On_LostEntity();

	if (O)
		O->On_SetEntity();

	pCurrentEntity = pCurrentViewEntity = O;
}

void IGame_Level::SetViewEntity(CObject* O)
{
	if (pCurrentViewEntity)
		pCurrentViewEntity->On_LostEntity();

	if (O)
		O->On_SetEntity();

	pCurrentViewEntity = O;
}

void IGame_Level::SoundEvent_Register(ref_sound_data_ptr S, float range)
{
	PROF_EVENT("IGame_Level::SoundEvent_Register");
	if (!g_bLoaded) return;
	if (!S) return;
	if (S->g_object && S->g_object->getDestroy())
	{
		S->g_object = 0;
		return;
	}
	if (0 == S->feedback) return;

	clamp(range, 0.1f, 500.f);

	const CSound_params* p = S->feedback->get_params();
	Fvector snd_position = p->position;
	if (S->feedback->is_2D())
	{
		snd_position.add(Sound->listener_position());
	}

	VERIFY(p && _valid(range));
	range = _min(range, p->max_ai_distance);
	VERIFY(_valid(snd_position));
	VERIFY(_valid(p->max_ai_distance));
	VERIFY(_valid(p->volume));

	// Query objects
	Fvector bb_size = {range, range, range};
	g_SpatialSpace->q_box(snd_ER, 0, STYPE_REACTTOSOUND, snd_position, bb_size);

	// Iterate
	auto it = snd_ER.begin();
	auto end = snd_ER.end();
	for (; it != end; it++)
	{
		Feel::Sound* L = (*it)->dcast_FeelSound();
		if (0 == L) continue;
		CObject* CO = (*it)->dcast_CObject();
		VERIFY(CO);
		if (CO->getDestroy()) continue;

		// Energy and signal
		VERIFY(_valid((*it)->spatial.sphere.P));
		float dist = snd_position.distance_to((*it)->spatial.sphere.P);
		if (dist > p->max_ai_distance) continue;
		VERIFY(_valid(dist));
		VERIFY2(!fis_zero(p->max_ai_distance), S->handle->file_name());
		float Power = (1.f - dist / p->max_ai_distance) * p->volume;
		VERIFY(_valid(Power));
		if (Power > EPS_S)
		{
			float occ = Sound->get_occlusion_to((*it)->spatial.sphere.P, snd_position);
			VERIFY(_valid(occ));
			Power *= occ;
			if (Power > EPS_S)
			{
				_esound_delegate D = {L, S, Power};
				snd_Events.push_back(D);
			}
		}
	}
	snd_ER.clear_not_free();
}

void IGame_Level::SoundEvent_Dispatch()
{
	PROF_EVENT("IGame_Level::SoundEvent_Dispatch");
	while (!snd_Events.empty())
	{
		_esound_delegate& D = snd_Events.back();
		VERIFY(D.dest && D.source);
		if (D.source->feedback)
		{
			D.dest->feel_sound_new(
				D.source->g_object,
				D.source->g_type,
				D.source->g_userdata,

				D.source->feedback->is_2D() ? Device.vCameraPosition : D.source->feedback->get_params()->position,
				D.power
			);
		}
		snd_Events.pop_back();
	}
}

// Lain: added
void IGame_Level::SoundEvent_OnDestDestroy(Feel::Sound* obj)
{
	PROF_EVENT("IGame_Level::SoundEvent_OnDestDestroy");
	struct rem_pred
	{
		rem_pred(Feel::Sound* obj) : m_obj(obj)
		{
		}

		bool operator ()(const _esound_delegate& d)
		{
			return d.dest == m_obj;
		}

	private:
		Feel::Sound* m_obj;
	};

	snd_Events.erase(std::remove_if(snd_Events.begin(), snd_Events.end(), rem_pred(obj)),
	                 snd_Events.end());
}
