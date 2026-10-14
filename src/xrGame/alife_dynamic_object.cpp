////////////////////////////////////////////////////////////////////////////
//	Module 		: alife_dynamic_object.cpp
//	Created 	: 27.10.2005
//  Modified 	: 27.10.2005
//	Author		: Dmitriy Iassenev
//	Description : ALife dynamic object class
////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "xrServer_Objects_ALife.h"
#include "xrServer_Objects_ALife_Monsters.h"
#include "alife_simulator.h"
#include "alife_schedule_registry.h"
#include "alife_graph_registry.h"
#include "alife_object_registry.h"
#include "level_graph.h"
#include "game_level_cross_table.h"
#include "game_graph.h"
#include "xrServer.h"
#include "level.h"
#include "map_manager.h"
#include "ai_space.h"
#include "../xrEngine/IGame_Level.h"

void CSE_ALifeDynamicObject::on_spawn()
{
#ifdef DEBUG
	//	Msg			("[LSS] spawning object [%d][%d][%s][%s]",ID,ID_Parent,name(),name_replace());
#endif
}

void CSE_ALifeDynamicObject::on_register()
{
	CSE_ALifeObject* object = this;
	while (object->ID_Parent != ALife::_OBJECT_ID(-1))
	{
		object = ai().alife().objects().object(object->ID_Parent);
		VERIFY(object);
	}

	if (!alife().graph().level().object(object->ID, true))
		clear_client_data();

    ::luabind::functor<void> funct;
    if (ai().script_engine().functor("_G.CSE_ALifeDynamicObject_on_register", funct))
        funct((u16)ID);
}

void CSE_ALifeDynamicObject::on_before_register()
{
}

void CSE_ALifeDynamicObject::on_unregister()
{
	::luabind::functor<void> funct;
	if (ai().script_engine().functor("_G.CSE_ALifeDynamicObject_on_unregister", funct))
		funct((u16)ID);
	Level().MapManager().OnObjectDestroyNotify(ID);
}

void CSE_ALifeDynamicObject::switch_online()
{
	R_ASSERT(!m_bOnline);
	m_bOnline = true;
	alife().add_online(this);
}

void CSE_ALifeDynamicObject::switch_offline()
{
	R_ASSERT(m_bOnline);
	m_bOnline = false;
	alife().remove_online(this);

	clear_client_data();
}

void CSE_ALifeDynamicObject::add_online(const bool& update_registries)
{
	if (!update_registries)
		return;

	alife().scheduled().remove(this);
	alife().graph().remove(this, m_tGraphID, false);
}

void CSE_ALifeDynamicObject::add_offline(const xr_vector<ALife::_OBJECT_ID>& saved_children,
                                         const bool& update_registries)
{
	if (!update_registries)
		return;

	alife().scheduled().add(this);
	alife().graph().add(this, m_tGraphID, false);
}

bool CSE_ALifeDynamicObject::synchronize_location()
{
	if (!ai().level_graph().valid_vertex_id(m_tNodeID)) return false;

	if (!ai().level_graph().valid_vertex_position(o_Position) || ai().level_graph().inside(
		ai().level_graph().vertex(m_tNodeID),
		o_Position))
		return (true);

	u32 const new_vertex_id = ai().level_graph().vertex(m_tNodeID, o_Position);
	if (!m_bOnline && !ai().level_graph().inside(new_vertex_id, o_Position))
		return (true);

	m_tNodeID = new_vertex_id;
	GameGraph::_GRAPH_ID tGraphID = ai().cross_table().vertex(m_tNodeID).game_vertex_id();
	if (tGraphID != m_tGraphID)
	{
		if (!m_bOnline)
		{
			Fvector position = o_Position;
			u32 level_vertex_id = m_tNodeID;
			alife().graph().change(this, m_tGraphID, tGraphID);
			if (ai().level_graph().inside(ai().level_graph().vertex(level_vertex_id), position))
			{
				level_vertex_id = m_tNodeID;
				o_Position = position;
			}
		}
		else
		{
			VERIFY(ai().game_graph().vertex(tGraphID)->level_id() == alife().graph().level().level_id());
			m_tGraphID = tGraphID;
		}
	}

	m_fDistance = ai().cross_table().vertex(m_tNodeID).distance();

	return (true);
}

// Seamless world experiment (kt): how far the object is from the player for the second radius, the
// one of the objects that never leave by themselves. A zone counts from its edge, not its middle.
// A negative answer: the radius is off.
static float kt_static_distance(CSE_ALifeDynamicObject* object, const Fvector& actor)
{
	if (g_kt_chunk.static_radius <= 0.f || !ai().game_graph().kt_placed())
		return (-1.f);
	if (object->ID == 0)
		return (-1.f);
	float distance = actor.distance_to(object->o_Position);
	if (CSE_Shape* shape = smart_cast<CSE_Shape*>(object))
	{
		float extent = 0.f;
		for (u32 i = 0; i < shape->shapes.size(); i++)
		{
			const CShapeData::shape_def& def = shape->shapes[i];
			float reach = (def.type == CShapeData::cfSphere)
				              ? def.data.sphere.P.magnitude() + def.data.sphere.R
				              : def.data.box.c.magnitude() + def.data.box.i.magnitude() + def.data.box.j.magnitude() + def.data.box.k.magnitude();
			if (reach > extent) extent = reach;
		}
		distance -= extent;
	}
	return (_max(distance, 0.f));
}

// Seamless world experiment (kt): the land the object stands on is not there - the geometry of its
// level is let go or is about to be. Nothing comes online over nothing, and what is online leaves.
// Until now only the distances kept that true: a level went further away than anything stays online.
bool kt_land_absent(const CSE_ALifeDynamicObject* object)
{
	if (!g_kt_chunk.active || object->ID == 0 || object->ID_Parent != 0xffff || !ai().get_level_graph())
		return (false);
	const CLevelGraph& graph = ai().level_graph();
	if (!graph.kt_own_vertex_count() || !graph.valid_vertex_id(object->m_tNodeID))
		return (false);
	const u8 piece = graph.kt_piece_of(object->m_tNodeID);
	return (piece != 255 && !g_kt_chunk.present(piece));
}

void CSE_ALifeDynamicObject::try_switch_online()
{
	CSE_ALifeSchedulable* schedulable = smart_cast<CSE_ALifeSchedulable*>(this);
	// checking if the abstract monster has just died
	if (schedulable)
	{
		if (!schedulable->need_update(this))
		{
			if (alife().scheduled().object(ID, true))
				alife().scheduled().remove(this);
		}
		else if (!alife().scheduled().object(ID, true))
			alife().scheduled().add(this);
	}

	if (!can_switch_online() || kt_land_absent(this))
	{
		on_failed_switch_online();
		return;
	}

	if (!can_switch_offline())
	{
		const float kt_distance = kt_static_distance(this, alife().graph().actor()->o_Position);
		if (kt_distance > g_kt_chunk.static_radius * .9f)
		{
			on_failed_switch_online();
			return;
		}
		alife().switch_online(this);
		return;
	}

	if (alife().graph().actor()->o_Position.distance_to(o_Position) > alife().online_distance())
	{
		on_failed_switch_online();
		return;
	}

	alife().switch_online(this);
}

void CSE_ALifeDynamicObject::try_switch_offline()
{
	// seamless world experiment (kt): a member of a squad that is online does not go offline on its
	// own - the squad switches its members. The escort ecologist's squad, held online by the scripts,
	// had its member taken offline by the distance alone, over and over (2026-10-14).
	if (g_kt_chunk.active)
		if (CSE_ALifeMonsterAbstract* monster = smart_cast<CSE_ALifeMonsterAbstract*>(this))
			if (monster->m_group_id != 0xffff)
				if (CSE_ALifeOnlineOfflineGroup* squad = smart_cast<CSE_ALifeOnlineOfflineGroup*>(ai().alife().objects().object(monster->m_group_id, true)))
					if (squad->m_bOnline)
						return;

	if (kt_land_absent(this))
	{
		alife().switch_offline(this);
		return;
	}

	if (!can_switch_offline())
	{
		if (can_switch_online() && kt_static_distance(this, alife().graph().actor()->o_Position) > g_kt_chunk.static_radius * 1.1f && g_kt_chunk.static_radius > 0.f)
			alife().switch_offline(this);
		return;
	}

	if (!can_switch_online())
	{
		alife().switch_offline(this);
		return;
	}

	if (alife().graph().actor()->o_Position.distance_to(o_Position) <= alife().offline_distance())
		return;

	alife().switch_offline(this);
}

bool CSE_ALifeDynamicObject::redundant() const
{
	return (false);
}

/// ---------------------------- CSE_ALifeInventoryBox ---------------------------------------------

void CSE_ALifeInventoryBox::add_online(const bool& update_registries)
{
	CSE_ALifeDynamicObjectVisual* object = (this);

	NET_Packet tNetPacket;
	ClientID clientID;
	clientID.set(
		object->alife().server().GetServerClient() ? object->alife().server().GetServerClient()->ID.value() : 0);

	ALife::OBJECT_IT I = object->children.begin();
	ALife::OBJECT_IT E = object->children.end();
	for (; I != E; ++I)
	{
		CSE_ALifeDynamicObject* l_tpALifeDynamicObject = ai().alife().objects().object(*I);
		CSE_ALifeInventoryItem* l_tpALifeInventoryItem = smart_cast<CSE_ALifeInventoryItem*>(l_tpALifeDynamicObject);
		R_ASSERT2(l_tpALifeInventoryItem, "Non inventory item object has parent?!");
		l_tpALifeInventoryItem->base()->s_flags.or(M_SPAWN_UPDATE);
		CSE_Abstract* l_tpAbstract = smart_cast<CSE_Abstract*>(l_tpALifeInventoryItem);
		object->alife().server().entity_Destroy(l_tpAbstract);

#ifdef DEBUG
		//		if (psAI_Flags.test(aiALife))
//			Msg					("[LSS] Spawning item [%s][%s][%d]",l_tpALifeInventoryItem->base()->name_replace(),*l_tpALifeInventoryItem->base()->s_name,l_tpALifeDynamicObject->ID);
		Msg						(
			"[LSS][%d] Going online [%d][%s][%d] with parent [%d][%s] on '%s'",
			Device.dwFrame,
			Device.dwTimeGlobal,
			l_tpALifeInventoryItem->base()->name_replace(),
			l_tpALifeInventoryItem->base()->ID,
			ID,
			name_replace(),
			"*SERVER*"
		);
#endif

		l_tpALifeDynamicObject->o_Position = object->o_Position;
		l_tpALifeDynamicObject->m_tNodeID = object->m_tNodeID;
		object->alife().server().Process_spawn(tNetPacket, clientID,FALSE, l_tpALifeInventoryItem->base());
		l_tpALifeDynamicObject->s_flags.and(u16(-1) ^ M_SPAWN_UPDATE);
		l_tpALifeDynamicObject->m_bOnline = true;
	}

	CSE_ALifeDynamicObjectVisual::add_online(update_registries);
}

void CSE_ALifeInventoryBox::add_offline(const xr_vector<ALife::_OBJECT_ID>& saved_children,
                                        const bool& update_registries)
{
	CSE_ALifeDynamicObjectVisual* object = (this);

	for (u32 i = 0, n = saved_children.size(); i < n; ++i)
	{
		CSE_ALifeDynamicObject* child = smart_cast<CSE_ALifeDynamicObject*>(
			ai().alife().objects().object(saved_children[i], true));
		// R_ASSERT(child);
		if (!child)
		{
			Msg("[DO] can't switch child [%d] offline, it's null", saved_children[i]);
			continue;
		}
		child->m_bOnline = false;

		CSE_ALifeInventoryItem* inventory_item = smart_cast<CSE_ALifeInventoryItem*>(child);
		VERIFY2(inventory_item, "Non inventory item object has parent?!");
#ifdef DEBUG
		//		if (psAI_Flags.test(aiALife))
//			Msg					("[LSS] Destroying item [%s][%s][%d]",inventory_item->base()->name_replace(),*inventory_item->base()->s_name,inventory_item->base()->ID);
		Msg						(
			"[LSS][%d] Going offline [%d][%s][%d] with parent [%d][%s] on '%s'",
			Device.dwFrame,
			Device.dwTimeGlobal,
			inventory_item->base()->name_replace(),
			inventory_item->base()->ID,
			ID,
			name_replace(),
			"*SERVER*"
		);
#endif

		ALife::_OBJECT_ID item_id = inventory_item->base()->ID;
		inventory_item->base()->ID = object->alife().server().PerformIDgen(item_id);

		if (!child->can_save())
		{
			object->alife().release(child);
			--i;
			--n;
			continue;
		}
		child->clear_client_data();
		object->alife().graph().add(child, child->m_tGraphID, false);
		//		object->alife().graph().attach	(*object,inventory_item,child->m_tGraphID,true);
		alife().graph().remove(child, child->m_tGraphID);
		children.push_back(child->ID);
		child->ID_Parent = ID;
	}


	CSE_ALifeDynamicObjectVisual::add_offline(saved_children, update_registries);
}

void CSE_ALifeDynamicObject::clear_client_data()
{
#ifdef DEBUG
	if (!client_data.empty())
		Msg						("CSE_ALifeDynamicObject::switch_offline: client_data is cleared for [%d][%s]",ID,name_replace());
#endif // DEBUG
	if (!keep_saved_data_anyway())
		client_data.clear();
}

void CSE_ALifeDynamicObject::on_failed_switch_online()
{
	clear_client_data();
}
