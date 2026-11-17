////////////////////////////////////////////////////////////////////////////
//	Module 		: alife_storage_manager.h
//	Created 	: 25.12.2002
//  Modified 	: 12.05.2004
//	Author		: Dmitriy Iassenev
//	Description : ALife Simulator storage manager
////////////////////////////////////////////////////////////////////////////

#pragma once

#include "alife_simulator_base.h"

class NET_Packet;

class CALifeStorageManager : public virtual CALifeSimulatorBase
{
	friend class CALifeUpdatePredicate;
protected:
	typedef CALifeSimulatorBase inherited;

protected:
	string_path m_save_name;
	LPCSTR m_section;

private:
	void prepare_objects_for_save();
	void load(void* buffer, const u32& buffer_size, LPCSTR file_name);

public:
	IC CALifeStorageManager(xrServer* server, LPCSTR section);
	virtual ~CALifeStorageManager();
	bool load(LPCSTR save_name = 0);
	void save(LPCSTR save_name = 0, bool update_name = true);
	void save(NET_Packet& net_packet);
	// seamless world experiment (kt): the objects of the levels standing next to the base one live in
	// the world's coordinates and node numbers, and are written to a save in their own level's
	void kt_move_objects(bool to_world);
	// kt (#514): an object of a save whose vertex this run's graph has not (one added by
	// -kt_graph_vertex of another build) is put on the nearest vertex of the world's base level
	void kt_lost_vertices(LPCSTR file_name);
};

#include "alife_storage_manager_inline.h"
