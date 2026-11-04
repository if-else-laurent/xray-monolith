#include "stdafx.h"
#include "game_sv_single.h"
#include "alife_simulator.h"
#include "xrServer_Objects.h"
#include "xrServer.h"
#include "xrmessages.h"
#include "ai_space.h"
#include "script_engine.h"

// kt (Anomaly, #352): the list of an object's children may name an item that is not its own on the server - one
// whose parent is another object or none, one not found, one named twice (GAMMA, a companion squad carried by
// teleport_object over a seam: the member's switch_offline met it in Perform_destroy and the assert in
// Perform_reject closed the game). Rejecting it would take it from an owner it does not have, and remove_online
// would make it the member's again offline: it is left out of the list and told of, where the owner is.
void xrServer::kt_drop_stray_children(CSE_Abstract* object, LPCSTR where)
{
	xr_vector<u16>& C = object->children;
	bool told = false;
	for (u32 i = 0; i < C.size();)
	{
		const u16 id = C[i];
		CSE_Abstract* child = game->get_entity_from_eid(id);
		const bool twice = std::find(C.begin(), C.begin() + i, id) != C.begin() + i;
		if (child && child->ID_Parent == object->ID && !twice)
		{
			++i;
			continue;
		}
		CSE_Abstract* owner = child && child->ID_Parent != 0xffff ? game->get_entity_from_eid(child->ID_Parent) : nullptr;
		const bool listed = owner && std::find(owner->children.begin(), owner->children.end(), id) != owner->children.end();
		Msg("! [kt] a stray child of [%s] id %u, section [%s], %s: [%s] id %u, section [%s], %s %u [%s]%s - left out (#352)",
		    object->name_replace(), u32(object->ID), object->s_name.c_str(), where,
		    child ? child->name_replace() : "?", u32(id), child ? child->s_name.c_str() : "?",
		    !child ? "not found, parent" : twice ? "named twice, parent" : "its parent is",
		    child ? u32(child->ID_Parent) : 0xffffu, owner ? owner->name_replace() : "none",
		    owner ? (listed ? ", which lists it" : ", which does not list it") : "");
		C.erase(C.begin() + i);
		told = true;
	}
	if (told)
		ai().script_engine().print_stack();
}

void xrServer::Perform_destroy(CSE_Abstract* object, u32 mode)
{
	R_ASSERT(object);
	if (object->ID_Parent != 0xffff)
	{
		// LPCSTR name = object->name();
		// if (name)
		// 	Msg("Attempt to destroy object %s with parent %d", name, object->ID_Parent);
		CSE_Abstract* parent = game->get_entity_from_eid(object->ID_Parent);
		if (parent)
			Perform_reject(object, parent, 2 * NET_Latency);
	}
	R_ASSERT(object->ID_Parent == 0xffff);

#ifdef DEBUG
#	ifdef SLOW_VERIFY_ENTITIES
		verify_entities			();
#	endif
#endif

	kt_drop_stray_children(object, "on destroy");
	while (!object->children.empty())
	{
		CSE_Abstract* child = game->get_entity_from_eid(object->children.back());
		// kt (#352): a child's reject runs scripts (game_sv_Single::OnDetach registers an item ALife did not know)
		if (!child || child->ID_Parent != object->ID)
		{
			kt_drop_stray_children(object, "while its children were destroyed");
			continue;
		}
		//		Msg					("SLS-CLEAR : REJECT  [%s][%s] FROM [%s][%s]",child->name(),child->name_replace(),object->name(),object->name_replace());
		Perform_reject(child, object, 2 * NET_Latency);
#ifdef DEBUG
#	ifdef SLOW_VERIFY_ENTITIES
		verify_entities			();
#	endif
#endif
		Perform_destroy(child, mode);
	}

	//	Msg						("SLS-CLEAR : DESTROY [%s][%s]",object->name(),object->name_replace());
	u16 object_id = object->ID;
	entity_Destroy(object);

#ifdef DEBUG
#	ifdef SLOW_VERIFY_ENTITIES
		verify_entities		();
#	endif
#endif

	NET_Packet P;
	P.w_begin(M_EVENT);
	P.w_u32(Device.dwTimeGlobal - 2 * NET_Latency);
	P.w_u16(GE_DESTROY);
	P.w_u16(object_id);
	SendBroadcast(BroadcastCID, P, mode);
}

void xrServer::SLS_Clear()
{
#if 0
	Msg									("SLS-CLEAR : %d objects");
	xrS_entities::const_iterator		I = entities.begin();
	xrS_entities::const_iterator		E = entities.end();
	for ( ; I != E; ++I)
		Msg								("entity to destroy : [%d][%s][%s]",(*I).second->ID,(*I).second->name(),(*I).second->name_replace());
#endif

	u32 mode = net_flags(TRUE,TRUE);
	while (!entities.empty())
	{
		bool found = false;
		xrS_entities::const_iterator I = entities.begin();
		xrS_entities::const_iterator E = entities.end();
		for (; I != E; ++I)
		{
			if ((*I).second->ID_Parent != 0xffff)
				continue;
			found = true;
			Perform_destroy((*I).second, mode);
			break;
		}
		if (!found) //R_ASSERT(found);
		{
			I = entities.begin();
			E = entities.end();
			for (; I != E; ++I)
			{
				if (I->second)
					Msg("! ERROR: can't destroy object [%d][%s] with parent [%d]",
					    I->second->ID, I->second->s_name.size() ? I->second->s_name.c_str() : "unknown",
					    I->second->ID_Parent
					);
				else
					Msg("! ERROR: can't destroy entity [%d][?] with parent[?]", I->first);
			}
			Msg("! ERROR: FATAL: can't delete all entities !");
			entities.clear();
		}
	}
}
