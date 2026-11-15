////////////////////////////////////////////////////////////////////////////
//	Module 		: stalker_animation_data_storage.cpp
//	Created 	: 13.10.2005
//  Modified 	: 13.10.2005
//	Author		: Dmitriy Iassenev
//	Description : Stalker animation data storage
////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "stalker_animation_data_storage.h"
#include "stalker_animation_data.h"
#include "object_broker.h"
#include "../Include/xrRender/Kinematics.h"

CStalkerAnimationDataStorage* g_stalker_animation_data_storage = 0;

static CStalkerAnimationDataStorage::MOTIONS_KEY motions_key(IKinematicsAnimated* skeleton_animated)
{
	VERIFY(skeleton_animated);
	CStalkerAnimationDataStorage::MOTIONS_KEY key;
	for (u16 i = 0, n = skeleton_animated->LL_MotionsSlotCount(); i < n; ++i)
		key.push_back(skeleton_animated->LL_MotionsSlot(i).id());
	return key;
}

CStalkerAnimationDataStorage::~CStalkerAnimationDataStorage()
{
	clear();
}

void CStalkerAnimationDataStorage::clear()
{
	while (!m_objects.empty())
	{
		xr_delete(m_objects.back().second);
		m_objects.pop_back();
	}
}

const CStalkerAnimationData* CStalkerAnimationDataStorage::object(IKinematicsAnimated* skeleton_animated)
{
	MOTIONS_KEY key = motions_key(skeleton_animated);
	OBJECTS::const_iterator I = std::find_if(m_objects.begin(), m_objects.end(),
		[&key](const OBJECT& object) { return object.first == key; });
	if (I != m_objects.end())
		return ((*I).second);

	m_objects.push_back(std::make_pair(key, xr_new<CStalkerAnimationData>(skeleton_animated)));
	return (m_objects.back().second);
}
