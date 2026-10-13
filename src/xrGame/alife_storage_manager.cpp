////////////////////////////////////////////////////////////////////////////
//	Module 		: alife_storage_manager.cpp
//	Created 	: 25.12.2002
//  Modified 	: 12.05.2004
//	Author		: Dmitriy Iassenev
//	Description : ALife Simulator storage manager
////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "alife_storage_manager.h"
#include "alife_simulator_header.h"
#include "alife_time_manager.h"
#include "alife_spawn_registry.h"
#include "alife_object_registry.h"
#include "alife_graph_registry.h"
#include "alife_group_registry.h"
#include "alife_registry_container.h"
#include "xrserver.h"
#include "level.h"
#include "../xrEngine/x_ray.h"
#include "saved_game_wrapper.h"
#include "string_table.h"
#include "../xrEngine/igame_persistent.h"
#include "autosave_manager.h"
#include "ai_space.h"
#include "game_graph.h"
#include "level_graph.h"
#include "game_level_cross_table.h"
#include "../xrEngine/IGame_Level.h"
#include "xrServer_Objects_ALife.h"
#include "xrServer_Objects_ALife_Items.h"
//Alundaio
#ifdef ENGINE_LUA_ALIFE_STORAGE_MANAGER_CALLBACKS
#include "pch_script.h"
#include "../../xrServerEntities/script_engine.h"
#endif
//-Alundaio

extern XRCORE_API string_path g_bug_report_file;

using namespace ALife;
#ifdef ENGINE_LUA_ALIFE_STORAGE_MANAGER_CALLBACKS
 //Alundaio
#endif

extern string_path g_last_saved_game;

CALifeStorageManager::~CALifeStorageManager()
{
	*g_last_saved_game = 0;
}

void CALifeStorageManager::save(LPCSTR save_name_no_check, bool update_name)
{
	PROF_EVENT();
	LPCSTR game_saves_path = FS.get_path("$game_saves$")->m_Path;

	string_path save_name;
	strncpy_s(save_name, sizeof(save_name), save_name_no_check,
	          sizeof(save_name) - 5 - xr_strlen(SAVE_EXTENSION) - xr_strlen(game_saves_path));

	xr_strcpy(g_last_saved_game, save_name);

	string_path save;
	xr_strcpy(save, m_save_name);
	if (save_name)
	{
		strconcat(sizeof(m_save_name), m_save_name, save_name, SAVE_EXTENSION);
	}
	else
	{
		if (!xr_strlen(m_save_name))
		{
			Log("There is no file name specified!");
			return;
		}
	}

	//Alundaio: To get the savegame fname to make our own custom save states
#ifdef ENGINE_LUA_ALIFE_STORAGE_MANAGER_CALLBACKS
	::luabind::functor<void> funct1;
	if (ai().script_engine().functor("alife_storage_manager.CALifeStorageManager_before_save", funct1))
		funct1((LPCSTR)m_save_name);
#endif
	//-Alundaio

	u32 source_count;
	u32 dest_count;
	void* dest_data;
	{
		CMemoryWriter stream;
		header().save(stream);
		time_manager().save(stream);
		spawns().save(stream);
		kt_move_objects(false);
		objects().save(stream);
		kt_move_objects(true);
		registry().save(stream);

		source_count = stream.tell();
		void* source_data = stream.pointer();
		dest_count = rtc_csize(source_count);
		dest_data = xr_malloc(dest_count);
		dest_count = rtc_compress(dest_data, dest_count, source_data, source_count);
	}

	string_path temp;
	FS.update_path(temp, "$game_saves$", m_save_name);
	IWriter* writer = FS.w_open(temp);
	writer->w_u32(u32(-1));
	writer->w_u32(ALIFE_VERSION);

	writer->w_u32(source_count);
	writer->w(dest_data, dest_count);
	xr_free(dest_data);
	FS.w_close(writer);
#ifdef DEBUG
	Msg							("* Game %s is successfully saved to file '%s' (%d bytes compressed to %d)",m_save_name,temp,source_count,dest_count + 4);
#else // DEBUG
	Msg("* Game %s is successfully saved to file '%s'", m_save_name, temp);
#endif // DEBUG

	//Alundaio: To get the savegame fname to make our own custom save states
#ifdef ENGINE_LUA_ALIFE_STORAGE_MANAGER_CALLBACKS
	::luabind::functor<void> funct2;
	if (ai().script_engine().functor("alife_storage_manager.CALifeStorageManager_save", funct2))
		funct2((LPCSTR)m_save_name);
#endif
	//-Alundaio

	if (!update_name)
		xr_strcpy(m_save_name, save);
}

void CALifeStorageManager::load(void* buffer, const u32& buffer_size, LPCSTR file_name)
{
	//Alundaio: So we can get the fname to make our own custom save states
#ifdef ENGINE_LUA_ALIFE_STORAGE_MANAGER_CALLBACKS
	::luabind::functor<void> funct;
	if (ai().script_engine().functor("alife_storage_manager.CALifeStorageManager_load", funct))
		funct(file_name);
#endif
	//-Alundaio

	IReader source(buffer, buffer_size);
	header().load(source);
	time_manager().load(source);
	spawns().load(source, file_name);
	graph().on_load();
	objects().load(source);

	VERIFY(can_register_objects());
	can_register_objects(false);
	CALifeObjectRegistry::OBJECT_REGISTRY::iterator B = objects().objects().begin();
	CALifeObjectRegistry::OBJECT_REGISTRY::iterator E = objects().objects().end();
	CALifeObjectRegistry::OBJECT_REGISTRY::iterator I;
	for (I = B; I != E; ++I)
	{
		ALife::_OBJECT_ID id = (*I).second->ID;
		(*I).second->ID = server().PerformIDgen(id);
		VERIFY(id == (*I).second->ID);
		register_object((*I).second, false);
	}

	registry().load(source);

	can_register_objects(true);
	kt_move_objects(true);

	for (I = B; I != E; ++I)
		(*I).second->on_register();

	if (!g_pGameLevel)
		return;

	Level().autosave_manager().on_game_loaded();
}

// The physical state an object is saved with - where the bones of a box, a barrel, a thing on the
// ground lay - is kept beside its place and in the same coordinates, so it moves with the place.
// Until this was done the state stayed in the world's coordinates of the day of the save: after
// Marsh was put 156 m further south its boxes hung in the air where they used to stand. Such a
// state, found far from the object's place after loading, is dropped: the object stands where
// the server has it.
static void kt_move_state(CSE_ALifeDynamicObject* object, const Fvector& shift, bool to_world)
{
	const float far_sqr = 50.f * 50.f;
	if (CSE_PHSkeleton* skeleton = smart_cast<CSE_PHSkeleton*>(object))
	{
		PHNETSTATE_VECTOR& bones = skeleton->saved_bones.bones;
		for (u32 i = 0; i < bones.size(); i++)
		{
			bones[i].position.add(shift);
			bones[i].previous_position.add(shift);
		}
		skeleton->saved_bones.m_min.add(shift);
		skeleton->saved_bones.m_max.add(shift);
		if (to_world && !bones.empty() && bones[0].position.distance_to_sqr(object->o_Position) > far_sqr)
		{
			bones.clear();
			skeleton->_flags.set(CSE_PHSkeleton::flSavedData, FALSE);
		}
	}
	SPHNetState* state = 0;
	if (CSE_ALifeInventoryItem* item = smart_cast<CSE_ALifeInventoryItem*>(object)) state = &item->State;
	else if (CSE_ALifeObjectPhysic* thing = smart_cast<CSE_ALifeObjectPhysic*>(object)) state = &thing->State;
	if (state)
	{
		state->position.add(shift);
		state->previous_position.add(shift);
		if (to_world && state->position.distance_to_sqr(object->o_Position) > far_sqr)
			state->position = state->previous_position = object->o_Position;
	}
}

void CALifeStorageManager::kt_move_objects(bool to_world)
{
	if (!ai().get_game_graph() || !ai().game_graph().kt_placed() || !ai().get_level_graph()) return;
	const CGameGraph& game_graph = ai().game_graph();
	const CLevelGraph& level_graph = ai().level_graph();
	u32 moved = 0, lost = 0;
	CALifeObjectRegistry::OBJECT_REGISTRY::iterator I = objects().objects().begin();
	CALifeObjectRegistry::OBJECT_REGISTRY::iterator E = objects().objects().end();
	for (; I != E; ++I)
	{
		CSE_ALifeDynamicObject* object = (*I).second;
		// An object that is online keeps on the server the vertex and node it went online with,
		// however far it has walked since. Which level's coordinates it is saved in is told by the
		// vertex, so the vertex is brought up to the place first.
		if (!to_world && object->m_bOnline && object->ID_Parent == 0xffff && object->used_ai_locations() &&
			level_graph.valid_vertex_position(object->o_Position))
		{
			const u32 node = level_graph.vertex_id(object->o_Position);
			if (level_graph.valid_vertex_id(node))
			{
				object->m_tNodeID = node;
				object->m_tGraphID = ai().cross_table().vertex(node).game_vertex_id();
			}
		}
		const int chunk = game_graph.kt_chunk(object->m_tGraphID);
		if (chunk < 0) continue;
		const Fvector& offset = g_kt_chunk.pieces[chunk].offset;
		moved++;
		if (to_world)
		{
			object->o_Position.add(offset);
			kt_move_state(object, offset, true);
			u32 node = level_graph.kt_node(u32(chunk), object->m_tNodeID);
			if (node == u32(-1) && level_graph.valid_vertex_position(object->o_Position))
				node = level_graph.vertex_id(object->o_Position);
			if (!level_graph.valid_vertex_id(node)) lost++;
			object->m_tNodeID = node;
		}
		else
		{
			object->o_Position.sub(offset);
			Fvector back;
			back.invert(offset);
			kt_move_state(object, back, false);
			u32 from, local;
			if (level_graph.kt_local(object->m_tNodeID, from, local) && int(from) == chunk)
				object->m_tNodeID = local;
			else
			{
				object->m_tNodeID = u32(-1);
				lost++;
			}
		}
	}
	Msg("* [kt world] %u objects moved %s, %u without a node", moved, to_world ? "into the world" : "back to their levels for the save", lost);
}

bool CALifeStorageManager::load(LPCSTR save_name_no_check)
{
	LPCSTR game_saves_path = FS.get_path("$game_saves$")->m_Path;

	string_path save_name;
	strncpy_s(save_name, sizeof(save_name), save_name_no_check,
	          sizeof(save_name) - 5 - xr_strlen(SAVE_EXTENSION) - xr_strlen(game_saves_path));

	CTimer timer;
	timer.Start();

	string_path save;
	xr_strcpy(save, m_save_name);
	if (!save_name)
	{
		if (!xr_strlen(m_save_name))
			R_ASSERT2(false, "There is no file name specified!");
	}
	else
	{
		strconcat(sizeof(m_save_name), m_save_name, save_name, SAVE_EXTENSION);
	}
	string_path file_name;
	FS.update_path(file_name, "$game_saves$", m_save_name);

	xr_strcpy(g_last_saved_game, save_name);
	xr_strcpy(g_bug_report_file, file_name);

	IReader* stream;
	stream = FS.r_open(file_name);
	if (!stream)
	{
		Msg("* Cannot find saved game %s", file_name);
		xr_strcpy(m_save_name, save);
		return (false);
	}

	CHECK_OR_EXIT(CSavedGameWrapper::valid_saved_game(*stream),
	              make_string("%s\nSaved game version mismatch or saved game is corrupted",file_name));
	/*
		string512					temp;
		strconcat					(sizeof(temp),temp,CStringTable().translate("st_loading_saved_game").c_str()," \"",save_name,SAVE_EXTENSION,"\"");
		g_pGamePersistent->LoadTitle(temp);
	*/
	g_pGamePersistent->LoadTitle();

	unload();
	reload(m_section);

	u32 source_count = stream->r_u32();
	void* source_data = xr_malloc(source_count);
	rtc_decompress(source_data, source_count, stream->pointer(), stream->length() - 3 * sizeof(u32));
	FS.r_close(stream);
	load(source_data, source_count, file_name);
	xr_free(source_data);

	groups().on_after_game_load();

	VERIFY(graph().actor());

	Msg("* Game %s is successfully loaded from file '%s' (%.3fs)", save_name, file_name, timer.GetElapsed_sec());

	return (true);
}

void CALifeStorageManager::save(NET_Packet& net_packet)
{
	PROF_EVENT();
	prepare_objects_for_save();

	shared_str game_name;
	net_packet.r_stringZ(game_name);
	save(*game_name, !!net_packet.r_u8());
}

void CALifeStorageManager::prepare_objects_for_save()
{
	PROF_EVENT();
	Level().ClientSend();
	Level().ClientSave();
}
