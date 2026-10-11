////////////////////////////////////////////////////////////////////////////
//	Module 		: game_level_cross_table_inline.h
//	Created 	: 20.02.2003
//  Modified 	: 13.11.2003
//	Author		: Dmitriy Iassenev
//	Description : Cross table between game and level graphs inline functions
////////////////////////////////////////////////////////////////////////////

#pragma once

#ifdef AI_COMPILER
IC CGameLevelCrossTable::CGameLevelCrossTable(LPCSTR fName)
{
	m_tpCrossTableVFS	= FS.r_open(fName);
	R_ASSERT2			(m_tpCrossTableVFS,"Can't open cross table!");
	
	IReader				*chunk = m_tpCrossTableVFS->open_chunk(CROSS_TABLE_CHUNK_VERSION);
	R_ASSERT2			(chunk,"Cross table is corrupted!");
	chunk->r			(&m_tCrossTableHeader,sizeof(m_tCrossTableHeader));
	chunk->close		();
	
	R_ASSERT2			(m_tCrossTableHeader.version() == XRAI_CURRENT_VERSION,"Cross table version mismatch!");

	m_chunk				= m_tpCrossTableVFS->open_chunk(CROSS_TABLE_CHUNK_DATA);
	R_ASSERT2			(m_chunk,"Cross table is corrupted!");
	m_tpaCrossTable		= (CCell*)m_chunk->pointer();
}
#endif // AI_COMPILER

IC CGameLevelCrossTable::CGameLevelCrossTable(const void* buffer, const u32& buffer_size)
{
	Memory.mem_copy(&m_tCrossTableHeader, buffer, sizeof(m_tCrossTableHeader));
	buffer = (const u8*)buffer + sizeof(m_tCrossTableHeader);

	R_ASSERT2(m_tCrossTableHeader.version() == XRAI_CURRENT_VERSION, "Cross table version mismatch!");

	m_tpaCrossTable = (CCell*)buffer;
#ifndef AI_COMPILER
	m_kt_own = 0;
#endif
}

IC CGameLevelCrossTable::~CGameLevelCrossTable()
{
#ifndef AI_COMPILER
	xr_free(m_kt_own);
#endif
};

#ifndef AI_COMPILER
IC void CGameLevelCrossTable::kt_extend(u32 new_count, const xr_vector<u32>& like)
{
	u32 old_count = m_tCrossTableHeader.level_vertex_count();
	if (new_count <= old_count) return;
	CCell* cells = xr_alloc<CCell>(new_count);
	CopyMemory(cells, m_tpaCrossTable, old_count * sizeof(CCell));
	for (u32 i = old_count; i < new_count; i++)
		CopyMemory(cells + i, m_tpaCrossTable + like[i - old_count], sizeof(CCell));
	xr_free(m_kt_own);
	m_kt_own = cells;
	m_tpaCrossTable = cells;
	((u32*)&m_tCrossTableHeader)[1] = new_count; // dwNodeCount, the second field of the header
}
#endif

IC const CGameLevelCrossTable::CCell& CGameLevelCrossTable::vertex(u32 level_vertex_id) const
{
	VERIFY(level_vertex_id < header().level_vertex_count());
	return (m_tpaCrossTable[level_vertex_id]);
}

IC u32 CGameLevelCrossTable::CHeader::version() const
{
	return (dwVersion);
}

IC u32 CGameLevelCrossTable::CHeader::level_vertex_count() const
{
	return (dwNodeCount);
}

IC u32 CGameLevelCrossTable::CHeader::game_vertex_count() const
{
	return (dwGraphPointCount);
}

IC const xrGUID& CGameLevelCrossTable::CHeader::level_guid() const
{
	return (m_level_guid);
}

IC const xrGUID& CGameLevelCrossTable::CHeader::game_guid() const
{
	return (m_game_guid);
}

IC GameGraph::_GRAPH_ID CGameLevelCrossTable::CCell::game_vertex_id() const
{
	return (tGraphIndex);
}

IC float CGameLevelCrossTable::CCell::distance() const
{
	return (fDistance);
}

IC const CGameLevelCrossTable::CHeader& CGameLevelCrossTable::header() const
{
	return (m_tCrossTableHeader);
}
