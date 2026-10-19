////////////////////////////////////////////////////////////////////////////
//	Module 		: alife_simulator_script.cpp
//	Created 	: 25.12.2002
//  Modified 	: 13.05.2004
//	Author		: Dmitriy Iassenev
//	Description : ALife Simulator script export
////////////////////////////////////////////////////////////////////////////

#include "pch_script.h"
#include "alife_simulator.h"
#include "ai_space.h"
#include "alife_object_registry.h"
#include "alife_story_registry.h"
#include "script_engine.h"
#include "xrServer_Objects_ALife_Monsters.h"
#include "restriction_space.h"
#include "alife_graph_registry.h"
#include "alife_spawn_registry.h"
#include "alife_registry_container.h"
#include "xrServer.h"
#include "level.h"
#include "level_graph.h"
#include "game_graph.h"

#include <luabind/iterator_policy.hpp>
#include <luabind/iterator_pair_policy.hpp>

using namespace luabind;

typedef xr_vector<std::pair<shared_str, int>> STORY_PAIRS;
typedef STORY_PAIRS SPAWN_STORY_PAIRS;
LPCSTR _INVALID_STORY_ID = "INVALID_STORY_ID";
LPCSTR _INVALID_SPAWN_STORY_ID = "INVALID_SPAWN_STORY_ID";
STORY_PAIRS story_ids;
SPAWN_STORY_PAIRS spawn_story_ids;

CALifeSimulator* alife()
{
	return (const_cast<CALifeSimulator*>(ai().get_alife()));
}

CSE_ALifeDynamicObject* alife_object(const CALifeSimulator* self, ALife::_OBJECT_ID object_id)
{
	VERIFY(self);
	if (object_id == 0xffff)
	{
		Msg("alife():object(id) ! invalid id specified");
        ai().script_engine().print_stack();
		return (0);
	}
	return (self->objects().object(object_id, true));
}

bool valid_object_id(const CALifeSimulator* self, ALife::_OBJECT_ID object_id)
{
	VERIFY(self);
	return (object_id != 0xffff);
}

#ifdef DEBUG
CSE_ALifeDynamicObject *alife_object		(const CALifeSimulator *self, LPCSTR name)
{
	VERIFY			(self);
	
	for (CALifeObjectRegistry::OBJECT_REGISTRY::const_iterator it = self->objects().objects().begin(); it != self->objects().objects().end(); it++) {
		CSE_ALifeDynamicObject	*obj = it->second;
		if (xr_strcmp(obj->name_replace(),name) == 0)
			return	(it->second);
	}
	
	return			(0);
}
#endif // #ifdef DEBUG

CSE_ALifeDynamicObject* alife_object(const CALifeSimulator* self, ALife::_OBJECT_ID id, bool no_assert)
{
	VERIFY(self);
	return (self->objects().object(id, no_assert));
}

CSE_ALifeDynamicObject* alife_story_object(const CALifeSimulator* self, ALife::_STORY_ID id)
{
	return (self->story_objects().object(id, true));
}

template <typename _id_type>
void generate_story_ids(
	STORY_PAIRS& result,
	_id_type INVALID_ID,
	LPCSTR section_name,
	LPCSTR INVALID_ID_STRING,
	LPCSTR invalid_id_description,
	LPCSTR invalid_id_redefinition,
	LPCSTR duplicated_id_description
)
{
	result.clear();

    const CInifile* Ini = pGameIni;

	LPCSTR N, V;
    u32 k = 0;
    R_ASSERT(Ini->section_exist(section_name));

    result.reserve(Ini->line_count(section_name) + 1);
    while (Ini->r_line(section_name, k, &N, &V))
	{
        const shared_str& temp = Ini->r_string_wb(section_name, N);

		R_ASSERT3(!strchr(*temp,' '), invalid_id_description, *temp);
		R_ASSERT2(xr_strcmp(*temp,INVALID_ID_STRING), invalid_id_redefinition);

        for (const auto& story : result)
            R_ASSERT3(story.first != temp, duplicated_id_description, *temp);

		result.push_back(std::make_pair(*temp, atoi(N)));
        ++k;
	}

	result.push_back(std::make_pair(INVALID_ID_STRING, INVALID_ID));
}

void kill_entity0(CALifeSimulator* alife, CSE_ALifeMonsterAbstract* monster, const GameGraph::_GRAPH_ID& game_vertex_id)
{
	alife->kill_entity(monster, game_vertex_id, 0);
}

void kill_entity1(CALifeSimulator* alife, CSE_ALifeMonsterAbstract* monster)
{
	alife->kill_entity(monster, monster->m_tGraphID, 0);
}

void add_in_restriction(CALifeSimulator* alife, CSE_ALifeMonsterAbstract* monster, ALife::_OBJECT_ID id)
{
	alife->add_restriction(monster->ID, id, RestrictionSpace::eRestrictorTypeIn);
}

void add_out_restriction(CALifeSimulator* alife, CSE_ALifeMonsterAbstract* monster, ALife::_OBJECT_ID id)
{
	alife->add_restriction(monster->ID, id, RestrictionSpace::eRestrictorTypeOut);
}

void remove_in_restriction(CALifeSimulator* alife, CSE_ALifeMonsterAbstract* monster, ALife::_OBJECT_ID id)
{
	alife->remove_restriction(monster->ID, id, RestrictionSpace::eRestrictorTypeIn);
}

void remove_out_restriction(CALifeSimulator* alife, CSE_ALifeMonsterAbstract* monster, ALife::_OBJECT_ID id)
{
	alife->remove_restriction(monster->ID, id, RestrictionSpace::eRestrictorTypeOut);
}

u32 get_level_id(CALifeSimulator* self)
{
	// Seamless world experiment (kt): for scripts, the level of the world the actor stands on
	if (g_pGameLevel)
	{
		const u32 location = Level().kt_location_id();
		if (location != u32(-1)) // while the game is being loaded the level has no AI map yet
			return (location);
	}
	return (self->graph().level().level_id());
}

CSE_ALifeDynamicObject* CALifeSimulator__create(CALifeSimulator* self, ALife::_SPAWN_ID spawn_id)
{
	const CALifeSpawnRegistry::SPAWN_GRAPH::CVertex* vertex = ai().alife().spawns().spawns().vertex(spawn_id);
	THROW2(vertex, "Invalid spawn id!");

	CSE_ALifeDynamicObject* spawn = smart_cast<CSE_ALifeDynamicObject*>(&vertex->data()->object());
	THROW(spawn);

	CSE_ALifeDynamicObject* object;
	self->create(object, spawn, spawn_id);

	return (object);
}

// Seamless world experiment (kt). Scripts make things at a place, a node and a vertex of the world
// graph. In the world the place and the node are the world's; but configs written for one level
// (dynamic anomalies, squads' spawn points) give them in that level's own coordinates and node
// numbers, and for a level standing next to the base one those are other numbers. Which frame a
// triple is in can be told: a node of the vertex's own level, by the world's numbering, means the
// world's frame. Otherwise the node is taken for the level's own and moved, with the place.
// A piece of our own: a corridor's AI map (-kt_ai_piece) or a chunk no level of the world graph is.
static bool kt_own_piece(const CGameGraph& graph, u8 piece)
{
	if (piece == u8(255)) return true;
	if (piece == 0) return false; // the base level's
	for (u32 v = 0, n = graph.header().vertex_count(); v < n; v++)
		if (graph.kt_chunk(v) == int(piece) - 1) return false;
	return true;
}

void kt_to_world(LPCSTR what, Fvector& position, u32& level_vertex_id, GameGraph::_GRAPH_ID game_vertex_id)
{
	if (!ai().get_game_graph() || !ai().get_level_graph()) return;
	const CGameGraph& graph = ai().game_graph();
	if (!graph.kt_placed() || !graph.valid_vertex_id(game_vertex_id)) return;
	const int chunk = graph.kt_chunk(game_vertex_id);
	if (chunk < 0 || u32(chunk) >= g_kt_chunk.pieces.size()) return; // the base level's frame is the world's
	const CLevelGraph& nodes = ai().level_graph();
	const bool valid = nodes.valid_vertex_id(level_vertex_id);
	if (valid && nodes.kt_piece_of(level_vertex_id) == u8(chunk + 1)) return;
	// a node of a piece of our own under the place given is the world's: no level numbers it. A squad
	// put at the foot of our ramp to Darkscape (its vertex is Darkscape's) wrote "of no use" each time
	// and was left as given, which was right
	if (valid && kt_own_piece(graph, nodes.kt_piece_of(level_vertex_id)) &&
	    position.distance_to_xz(nodes.vertex_position(level_vertex_id)) < 2.f)
		return;
	const u32 node = nodes.kt_node(u32(chunk), level_vertex_id);
	if (node == u32(-1) || !nodes.valid_vertex_id(node))
	{
		Msg("! [kt world] %s at (%.1f %.1f %.1f): node %u is of no use on %s, left as given", what, VPUSH(position), level_vertex_id, g_kt_chunk.pieces[chunk].name);
		return;
	}
	const Fvector there = nodes.vertex_position(node);
	Fvector moved;
	moved.add(position, g_kt_chunk.pieces[chunk].offset);
	Fvector placed;
	if (valid && position.distance_to_xz(nodes.vertex_position(level_vertex_id)) < 1.5f)
		placed = there; // the place was read from the node, taken for the world's
	else if (moved.distance_to_xz(there) < 50.f)
		placed = moved;
	else
	{
		Msg("! [kt world] %s at (%.1f %.1f %.1f) node %u for a vertex of %s: neither the world's frame nor that level's, left as given",
		    what, VPUSH(position), level_vertex_id, g_kt_chunk.pieces[chunk].name);
		return;
	}
	Msg("* [kt world] %s given in the own coordinates of %s: (%.1f %.1f %.1f) node %u -> (%.1f %.1f %.1f) node %u",
	    what, g_kt_chunk.pieces[chunk].name, VPUSH(position), level_vertex_id, VPUSH(placed), node);
	position = placed;
	level_vertex_id = node;
}

CSE_Abstract* CALifeSimulator__spawn_item(CALifeSimulator* self, LPCSTR section, const Fvector& given_position, u32 level_vertex_id, GameGraph::_GRAPH_ID game_vertex_id)
{
	THROW(self);
	Fvector position = given_position;
	kt_to_world(section, position, level_vertex_id, game_vertex_id);
	return (self->spawn_item(section, position, level_vertex_id, game_vertex_id, ALife::_OBJECT_ID(-1)));
}

CSE_Abstract* CALifeSimulator__spawn_item2(CALifeSimulator* self, LPCSTR section, const Fvector& given_position, u32 level_vertex_id, GameGraph::_GRAPH_ID game_vertex_id, ALife::_OBJECT_ID id_parent)
{
	Fvector position = given_position;
	if (id_parent == ALife::_OBJECT_ID(-1))
	{
		kt_to_world(section, position, level_vertex_id, game_vertex_id); // seamless world experiment (kt)
		return (self->spawn_item(section, position, level_vertex_id, game_vertex_id, id_parent));
	}

	CSE_ALifeDynamicObject* object = ai().alife().objects().object(id_parent, true);
	if (!object)
	{
		Msg("! invalid parent id [%d] specified", id_parent);
		return (0);
	}

	if (!object->m_bOnline)
		return (self->spawn_item(section, position, level_vertex_id, game_vertex_id, id_parent));

	NET_Packet packet;
	packet.w_begin(M_SPAWN);
	packet.w_stringZ(section);

	CSE_Abstract* item = self->spawn_item(section, position, level_vertex_id, game_vertex_id, id_parent, false);
	item->Spawn_Write(packet,FALSE);
	self->server().FreeID(item->ID, 0);
	F_entity_Destroy(item);

	ClientID clientID;
	clientID.set(0xffff);

	u16 dummy;
	packet.r_begin(dummy);
	VERIFY(dummy == M_SPAWN);
	return (self->server().Process_spawn(packet, clientID));
}

//Alundaio: Allows to call alife():register(se_obj) manually afterward so that packet editing can be done safely when spawning object with a parent
CSE_Abstract* CALifeSimulator__spawn_item3(CALifeSimulator* self, LPCSTR section, const Fvector& position, u32 level_vertex_id, GameGraph::_GRAPH_ID game_vertex_id, ALife::_OBJECT_ID id_parent, bool reg = true)
{
	if (reg == true)
		return CALifeSimulator__spawn_item2(self, section, position, level_vertex_id, game_vertex_id, id_parent);

	if (id_parent == ALife::_OBJECT_ID(-1))
	{
		Fvector moved_position = position;
		kt_to_world(section, moved_position, level_vertex_id, game_vertex_id); // seamless world experiment (kt)
		return (self->spawn_item(section, moved_position, level_vertex_id, game_vertex_id, id_parent));
	}

	CSE_ALifeDynamicObject* object = ai().alife().objects().object(id_parent, true);
	if (!object)
	{
		Msg("! invalid parent id [%d] specified", id_parent);
		return (0);
	}

	if (!object->m_bOnline)
		return (self->spawn_item(section, position, level_vertex_id, game_vertex_id, id_parent));

	CSE_Abstract* item = self->spawn_item(section, position, level_vertex_id, game_vertex_id, id_parent, false);

	return (item);
}

// Seamless world experiment (kt): a thing made and not yet registered, for a script that gives it a
// name (cse_abstract:set_name), its custom data and the like before alife():register (reprocess_spawn)
// puts it into the simulation. create(..., 65535, false) registers a thing with no parent all the
// same - only an item for an online parent stays unregistered there - and a camp of ours came into
// the simulation named smart_terrain<id> with no config, and its jobs failed the game (2026-10-20).
// A method of its own, so that create stays as it is for everybody; is it there -
// alife_simulator.create_unregistered.
static CSE_Abstract* kt_create_unregistered(CALifeSimulator* self, LPCSTR section, const Fvector& position, u32 level_vertex_id, GameGraph::_GRAPH_ID game_vertex_id)
{
	Fvector moved_position = position;
	kt_to_world(section, moved_position, level_vertex_id, game_vertex_id);
	return (self->spawn_item(section, moved_position, level_vertex_id, game_vertex_id, ALife::_OBJECT_ID(-1), false));
}

CSE_Abstract* CALifeSimulator__spawn_ammo(CALifeSimulator* self, LPCSTR section, const Fvector& position, u32 level_vertex_id, GameGraph::_GRAPH_ID game_vertex_id, ALife::_OBJECT_ID id_parent, int ammo_to_spawn)
{
	//	if (id_parent == ALife::_OBJECT_ID(-1))
	//		return							(self->spawn_item(section,position,level_vertex_id,game_vertex_id,id_parent));
	CSE_ALifeDynamicObject* object = 0;
	if (id_parent != ALife::_OBJECT_ID(-1))
	{
		object = ai().alife().objects().object(id_parent, true);
		if (!object)
		{
			Msg("! invalid parent id [%d] specified", id_parent);
			return (0);
		}
	}

	if (!object || !object->m_bOnline)
	{
		CSE_Abstract* item = self->spawn_item(section, position, level_vertex_id, game_vertex_id, id_parent);

		CSE_ALifeItemAmmo* ammo = smart_cast<CSE_ALifeItemAmmo*>(item);
		THROW(ammo);
		THROW(ammo->m_boxSize >= ammo_to_spawn);
		ammo->a_elapsed = (u16)ammo_to_spawn;

		return (item);
	}

	NET_Packet packet;
	packet.w_begin(M_SPAWN);
	packet.w_stringZ(section);

	CSE_Abstract* item = self->spawn_item(section, position, level_vertex_id, game_vertex_id, id_parent, false);

	CSE_ALifeItemAmmo* ammo = smart_cast<CSE_ALifeItemAmmo*>(item);
	THROW(ammo);
	THROW(ammo->m_boxSize >= ammo_to_spawn);
	ammo->a_elapsed = (u16)ammo_to_spawn;

	item->Spawn_Write(packet,FALSE);
	self->server().FreeID(item->ID, 0);
	F_entity_Destroy(item);

	ClientID clientID;
	clientID.set(0xffff);

	u16 dummy;
	packet.r_begin(dummy);
	VERIFY(dummy == M_SPAWN);
	return (self->server().Process_spawn(packet, clientID));
}

ALife::_SPAWN_ID CALifeSimulator__spawn_id(CALifeSimulator* self, ALife::_SPAWN_STORY_ID spawn_story_id)
{
	return (((const CALifeSimulator *)self)->spawns().spawn_id(spawn_story_id));
}

void CALifeSimulator__release(CALifeSimulator* self, CSE_Abstract* object, bool)
{
	VERIFY(self);
	//	self->release						(object,true);

	THROW(object);
	if (!object) return;
	CSE_ALifeObject* alife_object = smart_cast<CSE_ALifeObject*>(object);
	THROW(alife_object);
	if (!alife_object) return;
	if (!alife_object->m_bOnline)
	{
		self->release(object, true);
		return;
	}

	// awful hack, for stohe only
	NET_Packet packet;
	packet.w_begin(M_EVENT);
	packet.w_u32(Level().timeServer());
	packet.w_u16(GE_DESTROY);
	packet.w_u16(object->ID);
	Level().Send(packet, net_flags(TRUE,TRUE));
}

LPCSTR get_level_name(const CALifeSimulator* self, int level_id)
{
	const GameGraph::LEVEL_MAP& levels = ai().game_graph().header().levels();
	GameGraph::LEVEL_MAP::const_iterator I = levels.find((GameGraph::_LEVEL_ID)level_id);
	if (I == levels.end())
	{
		return NULL;
	}
	LPCSTR result = *ai().game_graph().header().level((GameGraph::_LEVEL_ID)level_id).name();
	return (result);
}

CSE_ALifeCreatureActor* get_actor(const CALifeSimulator* self)
{
	THROW(self);
	return (self->graph().actor());
}

KNOWN_INFO_VECTOR* registry(const CALifeSimulator* self, const ALife::_OBJECT_ID& id)
{
	THROW(self);
	return (self->registry().get<CInfoPortionRegistry>().object(id, true));
}

bool has_info(const CALifeSimulator* self, const ALife::_OBJECT_ID& id, LPCSTR info_id)
{
	const KNOWN_INFO_VECTOR* known_info = registry(self, id);
	if (!known_info)
		return (false);

	static shared_str has_info;
	has_info = info_id;

	if (std::find_if(known_info->begin(), known_info->end(), CFindByIDPred(has_info)) == known_info->end())
		return (false);

	return (true);
}

bool dont_has_info(const CALifeSimulator* self, const ALife::_OBJECT_ID& id, LPCSTR info_id)
{
	THROW(self);
	// absurdly, but only because of scriptwriters needs
	return (!has_info(self, id, info_id));
}

void AlifeGiveInfo(const CALifeSimulator *alife, const ALife::_OBJECT_ID &id, LPCSTR info_id)
{
	KNOWN_INFO_VECTOR *known_info = alife->registry().get<CInfoPortionRegistry>().object(id, true);
	if (!known_info)
		return;

	static shared_str AlifeGiveInfo;
	AlifeGiveInfo = info_id;

	if (std::find_if(known_info->begin(), known_info->end(), CFindByIDPred(AlifeGiveInfo)) == known_info->end())
	{
		known_info->push_back(info_id);
	}

	return;
}

void AlifeRemoveInfo(const CALifeSimulator *alife, const ALife::_OBJECT_ID &id, LPCSTR info_id)
{
	KNOWN_INFO_VECTOR	*known_info = alife->registry().get<CInfoPortionRegistry>().object(id, true);
	if (!known_info)
		return;

	static shared_str AlifeRemoveInfo;
	AlifeRemoveInfo = info_id;

	known_info->erase(std::find_if(known_info->begin(), known_info->end(), CFindByIDPred(AlifeRemoveInfo)),known_info->end());
}

//Alundaio: teleport object
void teleport_object(CALifeSimulator* alife, ALife::_OBJECT_ID id, GameGraph::_GRAPH_ID game_vertex_id,
                     u32 level_vertex_id, const Fvector& given_position)
{
	Fvector position = given_position;
	kt_to_world("a teleported object", position, level_vertex_id, game_vertex_id); // seamless world experiment (kt)
	alife->teleport_object(id, game_vertex_id, level_vertex_id, position);
}

void IterateInfo(const CALifeSimulator* alife, const ALife::_OBJECT_ID& id, const ::luabind::functor<void>& functor)
{
	const KNOWN_INFO_VECTOR* known_info = registry(alife, id);
	if (!known_info)
		return;

	xr_vector<shared_str>::const_iterator I = known_info->begin();
	xr_vector<shared_str>::const_iterator E = known_info->end();
	for (; I != E; ++I)
		functor(id, (LPCSTR)(*I).c_str());
}

CSE_Abstract* reprocess_spawn(CALifeSimulator* self, CSE_Abstract* object)
{
	NET_Packet packet;
	packet.w_begin(M_SPAWN);
	packet.w_stringZ(object->s_name);

	object->Spawn_Write(packet, FALSE);
	self->server().FreeID(object->ID, 0);
	F_entity_Destroy(object);

	ClientID clientID;
	clientID.set(0xffff);

	u16 dummy;
	packet.r_begin(dummy);

	return (self->server().Process_spawn(packet, clientID));
}

CSE_Abstract* try_to_clone_object(CALifeSimulator* self, CSE_Abstract* object, LPCSTR section, const Fvector& position,
                                  u32 level_vertex_id, GameGraph::_GRAPH_ID game_vertex_id, ALife::_OBJECT_ID id_parent,
                                  bool bRegister = true)
{
	CSE_ALifeItemWeaponMagazined* wpnmag = smart_cast<CSE_ALifeItemWeaponMagazined*>(object);
	if (wpnmag)
	{
		CSE_Abstract* absClone = self->spawn_item(section, position, level_vertex_id, game_vertex_id, id_parent, false);
		if (!absClone)
			return (0);

		CSE_ALifeItemWeaponMagazined* clone = smart_cast<CSE_ALifeItemWeaponMagazined*>(absClone);
		if (!clone)
			return (0);

		clone->wpn_flags = wpnmag->wpn_flags;
		clone->m_addon_flags = wpnmag->m_addon_flags;
		clone->m_fCondition = wpnmag->m_fCondition;
		clone->ammo_type = wpnmag->ammo_type;
		clone->m_upgrades = wpnmag->m_upgrades;
		clone->set_ammo_elapsed(wpnmag->get_ammo_elapsed());

		return bRegister ? reprocess_spawn(self, absClone) : absClone;
		// (self->server().Process_spawn(packet, clientID));
	}

	return (0);
}

void set_objects_per_update(CALifeSimulator* self, u32 count)
{
	self->objects_per_update(count);
}

void set_process_time(CALifeSimulator* self, int micro)
{
	self->set_process_time(micro);
}

void force_update(CALifeSimulator* self)
{
	self->update_scheduled(true);
}

// demonized: iterate alife objects, functor style
void CALifeSimulator__iterate_objects(const CALifeSimulator* self, const luabind::functor<bool>& functor)
{
	const CALifeObjectRegistry& objects = self->objects();
	for (const auto& se_obj : objects.objects()) {
		if (functor(se_obj.second)) break;
	}
}

void CALifeSimulator__iterate_objects_without_actor(const CALifeSimulator* self, const luabind::functor<bool>& functor)
{
	const CALifeObjectRegistry& objects = self->objects();
	for (const auto& se_obj : objects.objects()) {
		if (se_obj.first != 0 && functor(se_obj.second)) break;
	}
}

struct alife_object_iterator {
	const CALifeObjectRegistry::OBJECT_REGISTRY* container;
	CALifeObjectRegistry::OBJECT_REGISTRY::const_iterator it;

	alife_object_iterator(const CALifeObjectRegistry::OBJECT_REGISTRY* c)
		: container(c), it(c ? c->begin() : CALifeObjectRegistry::OBJECT_REGISTRY::const_iterator()) {}

	CSE_ALifeDynamicObject* next() {
		if (!container || it == container->end())
			return nullptr;
		auto obj = it->second;
		++it;
		return obj;
	}
};

struct alife_object_without_actor_iterator {
	const CALifeObjectRegistry::OBJECT_REGISTRY* container;
	CALifeObjectRegistry::OBJECT_REGISTRY::const_iterator it;

	alife_object_without_actor_iterator(const CALifeObjectRegistry::OBJECT_REGISTRY* c)
		: container(c), it(c ? c->begin() : CALifeObjectRegistry::OBJECT_REGISTRY::const_iterator()) {}

	CSE_ALifeDynamicObject* next() {
		if (!container || it == container->end())
			return nullptr;
		if (it->first == 0) {
			++it;
			return next();
		}
		auto obj = it->second;
		++it;
		return obj;
	}
};

// demonized: iterate alife objects, for loop
alife_object_iterator alife_object_iter(const CALifeSimulator* self)
{
	VERIFY(self);
	const CALifeObjectRegistry& objects = self->objects();
	return alife_object_iterator(&objects.objects());
}
// demonized: iterate alife objects without actor, for loop
alife_object_without_actor_iterator alife_object_without_actor_iter(const CALifeSimulator* self)
{
	VERIFY(self);
	const CALifeObjectRegistry& objects = self->objects();
	return alife_object_without_actor_iterator(&objects.objects());
}

ALife::_OBJECT_ID alife_max_id(const CALifeSimulator* self)
{
	return self->objects().max_id;
}

ALife::_OBJECT_ID alife_object_count(const CALifeSimulator* self)
{
    return self->objects().objects().size();
}

::luabind::object alife_object_ids(const CALifeSimulator* self, const bool keytable = false, const bool withActor = false)
{
	VERIFY(self);

	::luabind::object result = ::luabind::newtable(ai().script_engine().lua());
	const CALifeObjectRegistry& objects = self->objects();
	int i = 1;
	for (const auto& [id, obj] : objects.objects())
	{
		if (!withActor && id == 0)
		{
			continue;
		}
		const int index = keytable ? id : i;
		result[index] = keytable ? true : id;
		i++;
	}
	return result;
}

::luabind::object alife_objects(const CALifeSimulator *self, const bool keytable = false, const bool withActor = false)
{
	VERIFY(self);
	
	::luabind::object result = ::luabind::newtable(ai().script_engine().lua());
	const CALifeObjectRegistry& objects = self->objects();
	int i = 1;
	for (const auto& [id, obj] : objects.objects())
	{
		if (!withActor && id == 0)
		{
			continue;
		}
		const int index = keytable ? id : i;
		result[index] = obj;
		i++;
	}
	return result;
}

xr_vector<u16>& get_children(const CALifeSimulator *self, CSE_Abstract *object)
{
	VERIFY(self);
	return object->children;
}

#pragma optimize("s",on)
void CALifeSimulator::script_register(lua_State* L)
{
	module(L)
	[
		class_<alife_object_iterator>("alife_object_iterator")
		.def("next", &alife_object_iterator::next)
		.def("__call", &alife_object_iterator::next), // for Lua for-loop

		class_<alife_object_without_actor_iterator>("alife_object_without_actor_iterator")
		.def("next", &alife_object_iterator::next)
		.def("__call", &alife_object_iterator::next), // for Lua for-loop

		class_<CALifeSimulator>("alife_simulator")
		.def("valid_object_id", &valid_object_id)
		.def("level_id", &get_level_id)
		.def("level_name", &get_level_name)
		.def("object", (CSE_ALifeDynamicObject *(*)(const CALifeSimulator*, ALife::_OBJECT_ID))(alife_object))
		.def("object", (CSE_ALifeDynamicObject *(*)(const CALifeSimulator*, ALife::_OBJECT_ID, bool))(alife_object))
		.def("story_object", (CSE_ALifeDynamicObject *(*)(const CALifeSimulator*, ALife::_STORY_ID))(alife_story_object))
		.def("set_switch_online", (void (CALifeSimulator::*)(ALife::_OBJECT_ID, bool))(&CALifeSimulator::set_switch_online))
		.def("set_switch_offline", (void (CALifeSimulator::*)(ALife::_OBJECT_ID, bool))(&CALifeSimulator::set_switch_offline))
		.def("set_interactive", (void (CALifeSimulator::*)(ALife::_OBJECT_ID, bool))(&CALifeSimulator::set_interactive))
		.def("kill_entity", &CALifeSimulator::kill_entity)
		.def("kill_entity", &kill_entity0)
		.def("kill_entity", &kill_entity1)
		.def("add_in_restriction", &add_in_restriction)
		.def("add_out_restriction", &add_out_restriction)
		.def("remove_in_restriction", &remove_in_restriction)
		.def("remove_out_restriction", &remove_out_restriction)
		.def("remove_all_restrictions", &CALifeSimulator::remove_all_restrictions)
		.def("create", &CALifeSimulator__create)
		.def("create", &CALifeSimulator__spawn_item2)
		.def("create", &CALifeSimulator__spawn_item)
		.def("create", &CALifeSimulator__spawn_item3)
		.def("create_unregistered", &kt_create_unregistered)
		.def("create_ammo", &CALifeSimulator__spawn_ammo)
		.def("release", &CALifeSimulator__release)
		.def("spawn_id", &CALifeSimulator__spawn_id)
		.def("actor", &get_actor)
		.def("has_info", &has_info)
		.def("dont_has_info", &dont_has_info)
		.def("give_info", &AlifeGiveInfo)
		.def("disable_info", &AlifeRemoveInfo)
		.def("switch_distance", &CALifeSimulator::switch_distance)
		.def("set_switch_distance", &CALifeSimulator::set_switch_distance)
		//Alundaio: renamed to set_switch_distance from switch_distance
		//Alundaio: extend alife simulator exports
		.def("teleport_object", &::teleport_object)
		.def("iterate_info", &IterateInfo)
		.def("clone_weapon", &try_to_clone_object)
		.def("register", &reprocess_spawn)
		.def("set_objects_per_update", &set_objects_per_update)
		.def("set_process_time", &set_process_time)
		.def("force_update", &force_update)
		.def("get_children", &get_children, return_stl_iterator)
		//Alundaio: END

		// demonized: iterate alife objects
        .def("object_count", &alife_object_count)
		.def("object_ids", &alife_object_ids)
		.def("objects", &alife_objects)
		.def("iterate_objects", &CALifeSimulator__iterate_objects)
		/*.def("iterate_objects_without_actor", &CALifeSimulator__iterate_objects_without_actor)
		.def("objects_iter", &alife_object_iter)
		.def("objects_without_actor_iter", &alife_object_without_actor_iter)*/
		.def("max_id", &alife_max_id)

		, def("alife", &alife)
	];

	{
		if (story_ids.empty())
			generate_story_ids(
				story_ids,
				INVALID_STORY_ID,
				"story_ids",
				"INVALID_STORY_ID",
				"Invalid story id description (contains spaces)!",
				"INVALID_STORY_ID redifinition!",
				"Duplicated story id description!"
			);

		::luabind::class_<class_exporter<CALifeSimulator>> instance("story_ids");

		for (const auto& pair : story_ids)
			instance = std::move(instance).enum_("_story_ids")[::luabind::value(*pair.first, pair.second)];

		::luabind::module(L)[std::move(instance)];
	}

	{
		if (spawn_story_ids.empty())
			generate_story_ids(
				spawn_story_ids,
				INVALID_SPAWN_STORY_ID,
				"spawn_story_ids",
				"INVALID_SPAWN_STORY_ID",
				"Invalid spawn story id description (contains spaces)!",
				"INVALID_SPAWN_STORY_ID redifinition!",
				"Duplicated spawn story id description!"
			);

		::luabind::class_<class_exporter<class_exporter<CALifeSimulator>>> instance("spawn_story_ids");

		for (const auto& pair : spawn_story_ids)
			instance = std::move(instance).enum_("_spawn_story_ids")[::luabind::value(*pair.first, pair.second)];

		::luabind::module(L)[std::move(instance)];
	}
}

#if 0//def DEBUG
struct dummy {
    int count;
    lua_State* state;
    int ref;
};

void CALifeSimulator::validate			()
{
	typedef CALifeSpawnRegistry::SPAWN_GRAPH::const_vertex_iterator	const_vertex_iterator;
	const_vertex_iterator		I = spawns().spawns().vertices().begin();
	const_vertex_iterator		E = spawns().spawns().vertices().end();
	for ( ; I != E; ++I) {
		::luabind::wrap_base		*base = smart_cast<::luabind::wrap_base*>(&(*I).second->data()->object());
		if (!base)
			continue;

		if (!base->m_self.m_impl)
			continue;

		dummy					*_dummy = (dummy*)((void*)base->m_self.m_impl);
		lua_State				**_state = &_dummy->state;
		VERIFY2					(
			base->m_self.state(),
			make_string(
				"0x%08x name[%s] name_replace[%s]",
				*(int*)&_state,
				(*I).second->data()->object().name(),
				(*I).second->data()->object().name_replace()
			)
		);
	}
}
#endif //DEBUG
