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

	// the file's memory is not ours to write into
	const u32 size = u32((u8*)m_cross_tables - (u8*)m_nodes);
	xr_free(m_kt_nodes);
	m_kt_nodes = xr_alloc<u8>(size);
	CopyMemory(m_kt_nodes, m_nodes, size);
	m_nodes = (CVertex*)m_kt_nodes;

	const u32 count = header().vertex_count();
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
	m_kt_placed = true;
	Msg("* [kt world] %u vertices of the world graph moved into the world, %u without a node", moved, lost);
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
