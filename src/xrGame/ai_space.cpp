////////////////////////////////////////////////////////////////////////////
//	Module 		: ai_space.h
//	Created 	: 12.11.2003
//  Modified 	: 12.11.2003
//	Author		: Dmitriy Iassenev
//	Description : AI space class
////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "game_graph.h"
#include "game_level_cross_table.h"
#include "level_graph.h"
#include "graph_engine.h"
#include "ef_storage.h"
#include "ai_space.h"
#include "cover_manager.h"
#include "cover_point.h"
#include "script_engine.h"
#include "patrol_path_storage.h"
#include "alife_simulator.h"
#include "moving_objects.h"
#include "doors_manager.h"
#include "../xrEngine/dedicated_server_only.h"
#include "../xrEngine/no_single.h"
#include "../xrEngine/IGame_Level.h"

extern ENGINE_API bool g_dedicated_server;

CAI_Space* g_ai_space = 0;

CAI_Space::CAI_Space()
{
	m_ef_storage = 0;
	m_game_graph = 0;
	m_graph_engine = 0;
	m_cover_manager = 0;
	m_level_graph = 0;
	m_alife_simulator = 0;
	m_patrol_path_storage = 0;
	m_script_engine = 0;
	m_moving_objects = 0;
	m_doors_manager = 0;
}

void CAI_Space::init()
{
	if (g_dedicated_server)
		return;

#ifndef NO_SINGLE
	VERIFY(!m_ef_storage);
	m_ef_storage = xr_new<CEF_Storage>();

	VERIFY(!m_graph_engine);
	m_graph_engine = xr_new<CGraphEngine>(1024);

	VERIFY(!m_cover_manager);
	m_cover_manager = xr_new<CCoverManager>();

	VERIFY(!m_patrol_path_storage);
	m_patrol_path_storage = xr_new<CPatrolPathStorage>();

	VERIFY(!m_moving_objects);
	m_moving_objects = xr_new<::moving_objects>();

#endif //#ifndef NO_SINGLE

	VERIFY(!m_script_engine);
	m_script_engine = xr_new<CScriptEngine>();
	script_engine().init();

#ifndef NO_SINGLE
	extern string4096 g_ca_stdout;
	setvbuf(stderr, g_ca_stdout,_IOFBF, sizeof(g_ca_stdout));
#endif //#ifndef NO_SINGLE
}

CAI_Space::~CAI_Space()
{
	unload();

	try
	{
		xr_delete(m_script_engine);
	}
	catch (...)
	{
	}

	xr_delete(m_doors_manager);
	xr_delete(m_moving_objects);
	xr_delete(m_patrol_path_storage);
	xr_delete(m_cover_manager);
	xr_delete(m_graph_engine);
	xr_delete(m_ef_storage);
	VERIFY(!m_game_graph);
}

void CAI_Space::load(LPCSTR level_name)
{
	VERIFY(m_game_graph);

	unload(true);

#ifdef DEBUG
	Memory.mem_compact		();
	size_t						mem_usage = Memory.mem_usage();
	CTimer					timer;
	timer.Start				();
#endif

	const CGameGraph::SLevel& current_level = game_graph().header().level(level_name);

	m_level_graph = xr_new<CLevelGraph>();
	game_graph().set_current_level(current_level.id());
	const_cast<CGameGraph&>(game_graph()).kt_unplace();
	if (m_level_graph->kt_own_vertex_count())
	{
		// seamless world experiment (kt): the nodes of the levels standing next to this one belong
		// to their own vertices of the world graph; the vertices, the patrol paths and (in A-Life)
		// the objects of those levels move into this level's coordinates and node numbers
		xr_vector<CGameLevelCrossTable::CCell> cells;
		game_graph().kt_cross_cells(level_graph(), cells);
		const_cast<CGameLevelCrossTable&>(cross_table()).kt_extend(level_graph().header().vertex_count(), &cells[0]);
		const_cast<CGameGraph&>(game_graph()).kt_place(level_graph());
		if (m_patrol_path_storage)
			m_patrol_path_storage->kt_place(game_graph(), level_graph());
	}
	R_ASSERT2(cross_table().header().level_guid() == level_graph().header().guid(),
	          "cross_table doesn't correspond to the AI-map");
	R_ASSERT2(cross_table().header().game_guid() == game_graph().header().guid(),
	          "graph doesn't correspond to the cross table");
	m_graph_engine = xr_new<CGraphEngine>(
		_max(
			game_graph().header().vertex_count(),
			level_graph().header().vertex_count()
		)
	);

	R_ASSERT2(current_level.guid() == level_graph().header().guid(), "graph doesn't correspond to the AI-map");

#ifdef DEBUG
	if (!xr_strcmp(current_level.name(),level_name))
		validate			(current_level.id());
#endif

	level_graph().level_id(current_level.id());
	m_cover_manager->compute_static_cover();
	m_moving_objects->on_level_load();

	VERIFY(!m_doors_manager);
	m_doors_manager = xr_new<::doors::manager>(ai().level_graph().header().box());

#ifdef DEBUG
	Msg						("* Loading ai space is successfully completed (%.3fs, %7.3f Mb)",timer.GetElapsed_sec(),float(Memory.mem_usage() - mem_usage)/1048576.0);
#endif
}

// Seamless world experiment (kt), the world graph's side. See game_graph.h.
void CGameGraph::kt_claim()
{
	m_kt_chunk.clear();
	m_kt_level.clear();
	m_kt_world.clear();
	m_kt_placed = false;
	if (!kt_chunk_keys() || !g_kt_chunk.base[0]) return;
	int base = -1;
	int chunk_of_level[256];
	for (u32 i = 0; i < 256; i++) chunk_of_level[i] = -1;
	GameGraph::LEVEL_MAP::const_iterator I = header().levels().begin(), E = header().levels().end();
	for (; I != E; ++I)
	{
		LPCSTR name = *(*I).second.name();
		if (0 == stricmp(name, g_kt_chunk.base)) base = int((*I).first);
		for (u32 n = 0; n < g_kt_chunk.pieces.size(); n++)
			if (0 == stricmp(name, g_kt_chunk.pieces[n].name)) chunk_of_level[(*I).first] = int(n);
	}
	if (base < 0) return;

	// the file's memory is not ours to write into; room after it for the edges added (-kt_graph_edge):
	// a vertex that gets one has its whole list written anew there
#pragma pack(push,1)
	struct kt_edge_rec { u16 vertex; float distance; };
#pragma pack(pop)
	const u32 count = header().vertex_count();
	const u32 size = u32((u8*)m_cross_tables - (u8*)m_nodes);
	u32 extra = 0;
	for (u32 n = 0; n < g_kt_chunk.graph_edges.size(); n++)
		for (int side = 0; side < 2; side++)
		{
			const u32 v = side ? g_kt_chunk.graph_edges[n].b : g_kt_chunk.graph_edges[n].a;
			if (v < count) extra += (u32(m_nodes[v].tNeighbourCount) + 1) * sizeof(kt_edge_rec);
		}
	xr_free(m_kt_nodes);
	m_kt_nodes = xr_alloc<u8>(size + extra);
	CopyMemory(m_kt_nodes, m_nodes, size);
	m_nodes = (CVertex*)m_kt_nodes;
	u32 tail = size;
	for (u32 n = 0; n < g_kt_chunk.graph_drops.size(); n++)
		for (int side = 0; side < 2; side++)
		{
			const u32 v = side ? g_kt_chunk.graph_drops[n].b : g_kt_chunk.graph_drops[n].a;
			const u32 to = side ? g_kt_chunk.graph_drops[n].a : g_kt_chunk.graph_drops[n].b;
			if (v >= count) continue;
			CVertex& V = m_nodes[v];
			kt_edge_rec* edges = (kt_edge_rec*)((u8*)m_nodes + V.dwEdgeOffset);
			for (u32 e = 0; e < V.tNeighbourCount; e++)
				if (edges[e].vertex == to)
				{
					edges[e] = edges[V.tNeighbourCount - 1];
					V.tNeighbourCount--;
					Msg("* [kt world] the world graph: edge %u -> %u is taken out (-kt_graph_drop)", v, to);
					break;
				}
		}
	for (u32 n = 0; n < g_kt_chunk.graph_edges.size(); n++)
		for (int side = 0; side < 2; side++)
		{
			const KtChunk::GraphEdge& G = g_kt_chunk.graph_edges[n];
			const u32 v = side ? G.b : G.a, to = side ? G.a : G.b;
			if (v >= count || to >= count) continue;
			CVertex& V = m_nodes[v];
			kt_edge_rec* edges = (kt_edge_rec*)((u8*)m_nodes + V.dwEdgeOffset);
			bool there = false;
			for (u32 e = 0; e < V.tNeighbourCount; e++)
				if (edges[e].vertex == to)
				{
					edges[e].distance = G.length;
					there = true;
				}
			if (!there && V.tNeighbourCount < 255)
			{
				kt_edge_rec* moved = (kt_edge_rec*)(m_kt_nodes + tail);
				for (u32 e = 0; e < V.tNeighbourCount; e++) moved[e] = edges[e];
				moved[V.tNeighbourCount].vertex = u16(to);
				moved[V.tNeighbourCount].distance = G.length;
				V.dwEdgeOffset = tail;
				V.tNeighbourCount++;
				tail += u32(V.tNeighbourCount) * sizeof(kt_edge_rec);
			}
			Msg("* [kt world] the world graph: edge %u -> %u, %.1f m, %s (-kt_graph_edge)", v, to, G.length, there ? "its length set" : "added");
		}

	m_kt_chunk.assign(count, s8(-1));
	m_kt_node.resize(count);
	m_kt_level.resize(count);
	m_kt_world.assign(256, u8(0));
	m_kt_world[base] = 1;
	for (u32 i = 0; i < 256; i++)
		if (chunk_of_level[i] >= 0) m_kt_world[i] = 1;
	u32 claimed = 0;
	for (u32 i = 0; i < count; i++)
	{
		m_kt_node[i] = m_nodes[i].tNodeID;
		m_kt_level[i] = m_nodes[i].tLevelID;
		const int chunk = chunk_of_level[m_nodes[i].tLevelID];
		if (chunk < 0) continue;
		m_kt_chunk[i] = s8(chunk);
		m_nodes[i].tLevelID = u8(base);
		claimed++;
	}
	Msg("* [kt world] %u vertices of the world graph now belong to %s", claimed, g_kt_chunk.base);

	// The edges between the world's levels. The points are still the levels' own here (kt_place
	// moves them). An edge whose ends meet in the world is a seam: the graph has it as long as the
	// levels are apart on the game's map (the path to Marsh 161 m, in the world 3), and an offline
	// squad stood at the seam for the time of walking that far. It gets its length in the world.
	// -kt_graph_seams: between the base level and the levels named, the other edges are dropped.
	const float meet = 100.f; // metres: the seams' edges are 3, 15 and 71 m long in the world, the rest 170 and more
	u32 dropped = 0, measured = 0;
#pragma pack(push,1)
	struct kt_edge { u16 vertex; float distance; };
#pragma pack(pop)
	for (u32 i = 0; i < count; i++)
	{
		CVertex& V = m_nodes[i];
		kt_edge* edges = (kt_edge*)((u8*)m_nodes + V.dwEdgeOffset);
		for (u32 e = 0; e < V.tNeighbourCount; e++)
		{
			const u32 to = edges[e].vertex;
			if (to >= count || m_kt_level[i] == m_kt_level[to] || !m_kt_world[m_kt_level[i]] || !m_kt_world[m_kt_level[to]]) continue;
			Fvector p = V.tLocalPoint, q = m_nodes[to].tLocalPoint;
			if (m_kt_chunk[i] >= 0) p.add(g_kt_chunk.pieces[m_kt_chunk[i]].offset);
			if (m_kt_chunk[to] >= 0) q.add(g_kt_chunk.pieces[m_kt_chunk[to]].offset);
			const float real = p.distance_to(q);
			if (real > meet) continue;
			Msg("* [kt world] the world graph: edge %u -> %u is a seam, its length %.1f m becomes %.1f m", i, to, edges[e].distance, real);
			edges[e].distance = real;
			measured++;
		}
	}
	if (measured) Msg("* [kt world] the world graph: %u edges of the seams got their length in the world", measured);
	if (g_kt_chunk.graph_seams.empty()) return;
	for (u32 i = 0; i < count; i++)
	{
		CVertex& V = m_nodes[i];
		kt_edge* edges = (kt_edge*)((u8*)m_nodes + V.dwEdgeOffset);
		for (u32 e = 0; e < V.tNeighbourCount;)
		{
			const u32 to = edges[e].vertex;
			const int a = m_kt_chunk[i], b = to < count ? m_kt_chunk[to] : -1;
			const bool in_world = to < count && m_kt_level[i] != m_kt_level[to] && m_kt_world[m_kt_level[i]] && m_kt_world[m_kt_level[to]];
			bool named = false;
			if (in_world)
				for (u32 n = 0; n < g_kt_chunk.graph_seams.size(); n++)
					for (int side = 0; side < 2; side++)
					{
						const int chunk = side ? b : a, other = side ? a : b;
						// the level named on one end, the base level or another named one on the other
						if (chunk >= 0 && 0 == stricmp(g_kt_chunk.pieces[chunk].name, *g_kt_chunk.graph_seams[n]) && other < 0) named = true;
					}
			if (named)
			{
				Fvector p = V.tLocalPoint, q = m_nodes[to].tLocalPoint;
				if (a >= 0) p.add(g_kt_chunk.pieces[a].offset);
				if (b >= 0) q.add(g_kt_chunk.pieces[b].offset);
				if (const float way = g_kt_chunk.graph_way(i, to))
				{
					Msg("* [kt world] the world graph: edge %u -> %u is a seam's way of our own, its length %.1f m becomes %.1f m", i, to, edges[e].distance, way);
					edges[e].distance = way;
					e++;
					continue;
				}
				if (p.distance_to(q) > meet)
				{
					Msg("* [kt world] the world graph: edge %u -> %u is dropped, its ends are %.0f m apart in the world", i, to, p.distance_to(q));
					edges[e] = edges[V.tNeighbourCount - 1];
					V.tNeighbourCount--;
					dropped++;
					continue;
				}
			}
			e++;
		}
	}
	Msg("* [kt world] the world graph: %u edges between levels that do not meet are dropped", dropped);
}

void CGameGraph::kt_place(const CLevelGraph& level_graph)
{
	if (m_kt_chunk.empty() || m_kt_placed) return;
	u32 moved = 0, lost = 0;
	for (u32 i = 0, count = header().vertex_count(); i < count; i++)
	{
		const int chunk = m_kt_chunk[i];
		if (chunk < 0) continue;
		m_nodes[i].tLocalPoint.add(g_kt_chunk.pieces[chunk].offset);
		u32 node = level_graph.kt_node(u32(chunk), m_kt_node[i]);
		if (!level_graph.valid_vertex_id(node) && level_graph.valid_vertex_position(m_nodes[i].tLocalPoint))
			node = level_graph.vertex_id(m_nodes[i].tLocalPoint);
		if (!level_graph.valid_vertex_id(node))
		{
			lost++;
			node = 0;
		}
		m_nodes[i].tNodeID = node;
		moved++;
	}
	// -kt_graph_at: the ends of a seam's way of our own stand where the sleeve is
	for (u32 n = 0; n < g_kt_chunk.graph_ats.size(); n++)
	{
		const KtChunk::GraphAt& A = g_kt_chunk.graph_ats[n];
		if (A.vertex >= header().vertex_count()) continue;
		const u32 node = level_graph.valid_vertex_position(A.point) ? level_graph.vertex_id(A.point) : u32(-1);
		if (!level_graph.valid_vertex_id(node))
		{
			Msg("! [kt world] the world graph: vertex %u is not put at %.1f, %.1f, %.1f: no node there", A.vertex, VPUSH(A.point));
			continue;
		}
		Msg("* [kt world] the world graph: vertex %u is put at %.1f, %.1f, %.1f (it was at %.1f, %.1f, %.1f), node %u", A.vertex,
		    VPUSH(A.point), VPUSH(m_nodes[A.vertex].tLocalPoint), node);
		m_nodes[A.vertex].tLocalPoint = A.point;
		m_nodes[A.vertex].tNodeID = node;
	}
	// The points of the vertices where those who die offline are put (death points): each has a
	// place and a node in its level's own numbers too. They lie after the edges, in our copy.
	u32 points = 0, points_lost = 0;
	xr_set<const CLevelPoint*> seen;
	R_ASSERT(sizeof(CLevelPoint) == sizeof(Fvector) + sizeof(u32) + sizeof(float));
	for (u32 i = 0, count = header().vertex_count(); i < count; i++)
	{
		const int chunk = m_kt_chunk[i];
		if (chunk < 0) continue;
		const_spawn_iterator I, E;
		begin_spawn(i, I, E);
		for (; I != E; ++I)
		{
			if (!seen.insert(I).second) continue;
			Fvector* place = (Fvector*)(void*)I;
			u32* node = (u32*)((u8*)(void*)I + sizeof(Fvector));
			place->add(g_kt_chunk.pieces[chunk].offset);
			u32 merged = level_graph.kt_node(u32(chunk), *node);
			if (!level_graph.valid_vertex_id(merged) && level_graph.valid_vertex_position(*place))
				merged = level_graph.vertex_id(*place);
			if (!level_graph.valid_vertex_id(merged))
			{
				points_lost++;
				merged = m_nodes[i].tNodeID;
			}
			*node = merged;
			points++;
		}
	}
	m_kt_placed = true;
	Msg("* [kt world] %u vertices of the world graph moved into the world, %u without a node; %u death points, %u without a node",
	    moved, lost, points, points_lost);
}

void CGameGraph::kt_cross_cells(const CLevelGraph& level_graph, xr_vector<CGameLevelCrossTable::CCell>& cells) const
{
	const u32 own = level_graph.kt_own_vertex_count();
	const u32 extra = level_graph.header().vertex_count() - own;
	const xr_vector<u32>& like = level_graph.kt_like();
	cells.resize(extra);
	// first as before: the cell of the base level's node it is reached from; the small pieces of
	// our own and whatever has no table keep it
	for (u32 i = 0; i < extra; i++)
		cells[i] = cross_table().vertex(like[i]);

	u32* table = m_cross_tables;
	GameGraph::LEVEL_MAP::const_iterator I = header().levels().begin(), E = header().levels().end();
	for (; I != E; ++I, table = (u32*)((u8*)table + *table))
	{
		int chunk = -1;
		for (u32 n = 0; n < g_kt_chunk.pieces.size(); n++)
			if (0 == stricmp(*(*I).second.name(), g_kt_chunk.pieces[n].name)) chunk = int(n);
		if (chunk < 0) continue;
		CGameLevelCrossTable theirs(table + 1, *table);
		u32 taken = 0, strangers = 0;
		for (u32 local = 0, count = theirs.header().level_vertex_count(); local < count; local++)
		{
			const u32 node = level_graph.kt_node(u32(chunk), local);
			if (node == u32(-1)) continue;
			cells[node - own] = theirs.vertex(local);
			if (kt_chunk(theirs.vertex(local).game_vertex_id()) != chunk) strangers++;
			taken++;
		}
		Msg("* [kt world] %s: %u of %u nodes got their own vertices of the world graph, %u point elsewhere",
		    g_kt_chunk.pieces[chunk].name, taken, theirs.header().level_vertex_count(), strangers);
	}

	// The nodes of the small pieces of our own (a seam's corridor, the land by the Clear Sky base):
	// the cell of the nearest node of a level, counted in steps over the pieces - the base level's
	// or an attached one's, whose cells are known by now. Before, they kept the cell of the base
	// level's node the walk first reached them from: the land west of the Clear Sky base on Marsh
	// said "Cordon", its nodes pointing to a vertex of Cordon's (2026-10-14).
	const u32 total = level_graph.header().vertex_count();
	xr_vector<u8> ours(extra, 0);
	for (u32 node = own; node < total; node++)
	{
		u32 chunk, local;
		if (!level_graph.kt_local(node, chunk, local)) ours[node - own] = 1;
	}
	xr_vector<u32> front, next;
	u32 moved = 0, from_base = 0, from_chunk = 0, unreached = 0;
	for (u32 node = 0; node < total; node++)
	{
		if (node >= own && ours[node - own]) continue;
		const CLevelGraph::CVertex* V = level_graph.vertex(node);
		for (int k = 0; k < 4; k++)
		{
			const u32 to = V->link(k);
			if (!level_graph.valid_vertex_id(to) || to < own || ours[to - own] != 1) continue;
			ours[to - own] = 2; // reached
			cells[to - own] = node < own ? cross_table().vertex(node) : cells[node - own];
			(node < own ? from_base : from_chunk)++;
			front.push_back(to);
		}
	}
	while (!front.empty())
	{
		next.clear();
		for (u32 n = 0; n < front.size(); n++)
		{
			const CLevelGraph::CVertex* V = level_graph.vertex(front[n]);
			for (int k = 0; k < 4; k++)
			{
				const u32 to = V->link(k);
				if (!level_graph.valid_vertex_id(to) || to < own || ours[to - own] != 1) continue;
				ours[to - own] = 2;
				cells[to - own] = cells[front[n] - own];
				next.push_back(to);
			}
		}
		front.swap(next);
	}
	// The land comes first: a node on a level's land (the world's mask) takes the nearest node of
	// that level, where a walk over the pieces leads to one. By nearness alone 3105 nodes of the
	// corridor on Cordon's land by the joint of the railways counted as Marsh: Cordon's own nodes
	// are far off there, below its embankment (2026-10-14).
	u32 by_land = 0;
	if (!g_kt_chunk.mask.empty())
	{
		const u32 groups = 1 + u32(g_kt_chunk.pieces.size());
		xr_vector<u8> land(extra, 255), done(extra, 0);
		for (u32 i = 0; i < extra; i++)
		{
			if (ours[i] != 2) continue;
			const Fvector P = level_graph.vertex_position(own + i);
			u32 owner = g_kt_chunk.mask_at(P.x, P.z);
			if (owner == 255) continue;
			owner &= 0x7f;
			if (owner >= 120) owner -= 120; // nobody's land between the levels, named by a level's grass
			if (owner < groups) land[i] = u8(owner);
		}
		for (u32 g = 0; g < groups; g++)
		{
			xr_vector<u8> seen(extra, 0);
			xr_vector<CGameLevelCrossTable::CCell> carried(extra);
			front.clear();
			for (u32 node = 0; node < total; node++)
			{
				if (node >= own && ours[node - own]) continue;
				u32 chunk = 0, local = 0;
				const u32 group = node < own ? 0 : (level_graph.kt_local(node, chunk, local) ? chunk + 1 : u32(-1));
				if (group != g) continue;
				const CLevelGraph::CVertex* V = level_graph.vertex(node);
				for (int k = 0; k < 4; k++)
				{
					const u32 to = V->link(k);
					if (!level_graph.valid_vertex_id(to) || to < own || ours[to - own] != 2 || seen[to - own]) continue;
					seen[to - own] = 1;
					carried[to - own] = node < own ? cross_table().vertex(node) : cells[node - own];
					front.push_back(to);
				}
			}
			while (!front.empty())
			{
				next.clear();
				for (u32 n = 0; n < front.size(); n++)
				{
					const u32 i = front[n] - own;
					if (land[i] == g && !done[i])
					{
						done[i] = 1;
						if (cells[i].game_vertex_id() != carried[i].game_vertex_id()) by_land++;
						cells[i] = carried[i];
					}
					const CLevelGraph::CVertex* V = level_graph.vertex(front[n]);
					for (int k = 0; k < 4; k++)
					{
						const u32 to = V->link(k);
						if (!level_graph.valid_vertex_id(to) || to < own || ours[to - own] != 2 || seen[to - own]) continue;
						seen[to - own] = 1;
						carried[to - own] = carried[i];
						next.push_back(to);
					}
				}
				front.swap(next);
			}
		}
	}
	for (u32 i = 0; i < extra; i++)
	{
		if (ours[i] == 1) unreached++;
		else if (ours[i] == 2 && cells[i].game_vertex_id() != cross_table().vertex(like[i]).game_vertex_id()) moved++;
	}
	Msg("* [kt world] nodes of our own pieces: the vertex of the nearest node of the level whose land they are on (else of any), "
	    "%u now point elsewhere than before, %u changed for the land; entered from the base level at %u, from attached ones at %u; "
	    "%u reached from no level",
	    moved, by_land, from_base, from_chunk, unreached);
}

void CAI_Space::unload(bool reload)
{
	if (g_dedicated_server)
		return;

	script_engine().unload();

	xr_delete(m_doors_manager);
	xr_delete(m_graph_engine);
	xr_delete(m_level_graph);

	if (!reload && m_game_graph)
		m_graph_engine = xr_new<CGraphEngine>(game_graph().header().vertex_count());
}

#ifdef DEBUG
void CAI_Space::validate			(const u32 level_id) const
{
	VERIFY					(level_graph().header().vertex_count() == cross_table().header().level_vertex_count());
	for (GameGraph::_GRAPH_ID i=0, n = game_graph().header().vertex_count(); i<n; ++i)
		if ((level_id == game_graph().vertex(i)->level_id()) && 
			(!level_graph().valid_vertex_id(game_graph().vertex(i)->level_vertex_id()) ||
			(cross_table().vertex(game_graph().vertex(i)->level_vertex_id()).game_vertex_id() != i) ||
			!level_graph().inside(game_graph().vertex(i)->level_vertex_id(),game_graph().vertex(i)->level_point()))) {
			Msg				("! Graph doesn't correspond to the cross table");
			R_ASSERT2		(false,"Graph doesn't correspond to the cross table");
		}

//	Msg						("death graph point id : %d",cross_table().vertex(455236).game_vertex_id());

	for (u32 i=0, n=game_graph().header().vertex_count(); i<n; ++i) {
		if (level_id != game_graph().vertex(i)->level_id())
			continue;

		CGameGraph::const_spawn_iterator	I, E;
		game_graph().begin_spawn			(i,I,E);
//		Msg									("vertex [%d] has %d death points",i,game_graph().vertex(i)->death_point_count());
		for ( ; I != E; ++I) {
			VERIFY							(cross_table().vertex((*I).level_vertex_id()).game_vertex_id() == i);
		}
	}
	

//	Msg						("* Graph corresponds to the cross table");
}
#endif

void CAI_Space::patrol_path_storage_raw(IReader& stream)
{
	if (g_dedicated_server)
		return;

	xr_delete(m_patrol_path_storage);
	m_patrol_path_storage = xr_new<CPatrolPathStorage>();
	m_patrol_path_storage->load_raw(get_level_graph(), get_cross_table(), get_game_graph(), stream);
	m_patrol_path_storage->load_from_config();
}

void CAI_Space::patrol_path_storage(IReader& stream)
{
	if (g_dedicated_server)
		return;

	xr_delete(m_patrol_path_storage);
	m_patrol_path_storage = xr_new<CPatrolPathStorage>();
	m_patrol_path_storage->load(stream);
	m_patrol_path_storage->load_from_config();
}

void CAI_Space::set_alife(CALifeSimulator* alife_simulator)
{
	VERIFY((!m_alife_simulator && alife_simulator) || (m_alife_simulator && !alife_simulator));
	m_alife_simulator = alife_simulator;

	VERIFY(!alife_simulator || !m_game_graph);
	if (alife_simulator)
		return;

	VERIFY(m_game_graph);
	m_game_graph = 0;
	xr_delete(m_graph_engine);
}

void CAI_Space::game_graph(CGameGraph* game_graph)
{
	VERIFY(m_alife_simulator);
	VERIFY(game_graph);
	VERIFY(!m_game_graph);
	m_game_graph = game_graph;

	//	VERIFY					(!m_graph_engine);
	xr_delete(m_graph_engine);
	m_graph_engine = xr_new<CGraphEngine>(this->game_graph().header().vertex_count());
}

const CGameLevelCrossTable& CAI_Space::cross_table() const
{
	return (game_graph().cross_table());
}

const CGameLevelCrossTable* CAI_Space::get_cross_table() const
{
	return (&game_graph().cross_table());
}
