////////////////////////////////////////////////////////////////////////////
//	Module 		: saved_game_wrapper.cpp
//	Created 	: 21.02.2006
//  Modified 	: 21.02.2006
//	Author		: Dmitriy Iassenev
//	Description : saved game wrapper class
////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "saved_game_wrapper.h"
#include "alife_time_manager.h"
#include "alife_object_registry.h"
#include "xrServer_Objects_ALife_Monsters.h"
#include "ai_space.h"
#include "game_graph.h"
#include "alife_simulator_header.h"
#include "alife_simulator.h"
#include "alife_spawn_registry.h"
#include "../xrEngine/IGame_Level.h"

extern LPCSTR alife_section;

LPCSTR CSavedGameWrapper::saved_game_full_name(LPCSTR saved_game_name, string_path& result)
{
	string_path temp;
	strconcat(sizeof(temp), temp, saved_game_name,SAVE_EXTENSION);
	FS.update_path(result, "$game_saves$", temp);
	return (result);
}

bool CSavedGameWrapper::saved_game_exist(LPCSTR saved_game_name)
{
	string_path file_name;
	return (!!FS.exist(saved_game_full_name(saved_game_name, file_name)));
}

bool CSavedGameWrapper::valid_saved_game(IReader& stream)
{
	if (stream.length() < 8)
		return (false);

	if (stream.r_u32() != u32(-1))
		return (false);

	if (stream.r_u32() < ALIFE_VERSION)
		return (false);

	return (true);
}

bool CSavedGameWrapper::valid_saved_game(LPCSTR saved_game_name)
{
	string_path file_name;
	if (!FS.exist(saved_game_full_name(saved_game_name, file_name)))
		return (false);

	IReader* stream = FS.r_open(file_name);
	bool result = valid_saved_game(*stream);
	FS.r_close(stream);
	return (result);
}

CSavedGameWrapper::CSavedGameWrapper(LPCSTR saved_game_name)
{
	string_path file_name;
	saved_game_full_name(saved_game_name, file_name);
	R_ASSERT3(FS.exist(file_name), "There is no saved game ", file_name);

	IReader* stream = FS.r_open(file_name);
	if (!valid_saved_game(*stream))
	{
		FS.r_close(stream);
		CALifeTimeManager time_manager(alife_section);
		m_game_time = time_manager.game_time();
		m_actor_health = 1.f;
		m_level_id = _LEVEL_ID(-1);
		m_level_name = "";
		return;
	}

	u32 source_count = stream->r_u32();
	void* source_data = xr_malloc(source_count);
	rtc_decompress(source_data, source_count, stream->pointer(), stream->length() - 3 * sizeof(u32));
	FS.r_close(stream);

	IReader reader(source_data, source_count);

	{
		CALifeTimeManager time_manager(alife_section);
		time_manager.load(reader);
		m_game_time = time_manager.game_time();
	}

	{
		R_ASSERT2(reader.find_chunk(OBJECT_CHUNK_DATA), "Can't find chunk OBJECT_CHUNK_DATA!");
		u32 count = reader.r_u32();
		VERIFY(count > 0);
		CSE_ALifeDynamicObject* object = CALifeObjectRegistry::get_object(reader);
		VERIFY(object->ID == 0);
		CSE_ALifeCreatureActor* actor = smart_cast<CSE_ALifeCreatureActor*>(object);
		VERIFY(actor);

		m_actor_health = actor->get_health();

		IReader* chunk = reader.open_chunk(SPAWN_CHUNK_DATA);
		R_ASSERT2(chunk, "Spawn version mismatch - REBUILD SPAWN!");

		string_path spawn_file_name;
		{
			IReader* sub_chunk = chunk->open_chunk(0);
			if (!sub_chunk)
			{
				chunk->close();
				F_entity_Destroy(object);
				m_level_id = _LEVEL_ID(-1);
				m_level_name = "";
				return;
			}
			sub_chunk->r_stringZ(spawn_file_name, sizeof(spawn_file_name));
			sub_chunk->close();
		}

		chunk->close();

		if (!FS.exist(file_name, "$game_spawn$", spawn_file_name, ".spawn"))
		{
			F_entity_Destroy(object);
			m_level_id = _LEVEL_ID(-1);
			m_level_name = "";
			return;
		}

		IReader* spawn = NULL;
		bool b_destroy_spawn = true;
		if (ai().get_alife() && ai().alife().spawns().get_spawn_name() == spawn_file_name)
		{
			spawn = ai().alife().spawns().get_spawn_file();
			b_destroy_spawn = false;
		}
		else
			spawn = FS.r_open(file_name);

		if (!spawn)
		{
			F_entity_Destroy(object);
			m_level_id = _LEVEL_ID(-1);
			m_level_name = "";
			return;
		}

		chunk = spawn->open_chunk(4);
		if (!chunk)
		{
			F_entity_Destroy(object);
			if (b_destroy_spawn)
				FS.r_close(spawn);
			m_level_id = _LEVEL_ID(-1);
			m_level_name = "";
			return;
		}

		{
			// kt (#514, Anomaly/Monolith): the graph here is the file's, as it is before kt_claim; the
			// vertices the world adds (-kt_graph_vertex, numbered after the file's) are not in it, and a
			// save of an actor standing at one read past the vertices: a level id of garbage, an assert in
			// the load dialog (level 67). Such a vertex gets its like's level, if this run adds it, else the
			// world's base level; otherwise, and for any level the header does not know, the save is
			// listed without a level, as one whose spawn is missing.
			CGameGraph graph(*chunk);
			const u32 vertex = object->m_tGraphID;
			u32 level = u32(-1);
			if (graph.valid_vertex_id(vertex))
				level = graph.vertex(vertex)->level_id();
			else
			{
				LPCSTR how = "no level";
				if (kt_chunk_keys())
				{
					for (u32 n = 0; n < g_kt_chunk.graph_vertices.size(); n++)
						if (g_kt_chunk.graph_vertices[n].vertex == vertex && graph.valid_vertex_id(g_kt_chunk.graph_vertices[n].like))
						{
							level = graph.vertex(g_kt_chunk.graph_vertices[n].like)->level_id();
							how = "its like's level";
							break;
						}
					if (level == u32(-1) && g_kt_chunk.base[0])
					{
						GameGraph::LEVEL_MAP::const_iterator I = graph.header().levels().begin(), E = graph.header().levels().end();
						for (; I != E; ++I)
							if (0 == stricmp(*(*I).second.name(), g_kt_chunk.base))
							{
								level = u32((*I).first);
								how = "the world's base level";
								break;
							}
					}
				}
				Msg("! [kt world] save %s: the actor's vertex %u is not in the file's graph of %u vertices, listed with %s",
				    saved_game_name, vertex, u32(graph.header().vertex_count()), how);
			}
			GameGraph::LEVEL_MAP::const_iterator L = graph.header().levels().find(GameGraph::_LEVEL_ID(level));
			if (level < 256 && L != graph.header().levels().end())
			{
				m_level_id = GameGraph::_LEVEL_ID(level);
				m_level_name = (*L).second.name();
			}
			else
			{
				m_level_id = _LEVEL_ID(-1);
				m_level_name = "";
			}
		}

		chunk->close();
		if (b_destroy_spawn)
			FS.r_close(spawn);
		F_entity_Destroy(object);
	}

	xr_free(source_data);
}
