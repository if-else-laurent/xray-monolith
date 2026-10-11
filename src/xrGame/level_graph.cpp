////////////////////////////////////////////////////////////////////////////
//	Module 		: level_graph.cpp
//	Created 	: 02.10.2001
//  Modified 	: 11.11.2003
//	Author		: Oles Shihkovtsov, Dmitriy Iassenev
//	Description : Level graph
////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "level_graph.h"
#include "profiler.h"
#ifndef AI_COMPILER
#	include "../xrEngine/IGame_Level.h"
#endif

LPCSTR LEVEL_GRAPH_NAME = "level.ai";

#ifdef AI_COMPILER
CLevelGraph::CLevelGraph		(LPCSTR filename)
#else
CLevelGraph::CLevelGraph()
#endif
{
#ifndef AI_COMPILER
#ifdef DEBUG
	sh_debug->create				("debug\\ai_nodes","$null");
#endif
	string_path file_name;
	FS.update_path(file_name, "$level$", LEVEL_GRAPH_NAME);
#else
	string256					file_name;
	strconcat					(sizeof(file_name), file_name, filename, LEVEL_GRAPH_NAME);
#endif
	m_reader = FS.r_open(file_name);

	// m_header & data
	m_header = (CHeader*)m_reader->pointer();
	R_ASSERT(header().version() == XRAI_CURRENT_VERSION);
	m_reader->advance(sizeof(CHeader));
	m_nodes = (CVertex*)m_reader->pointer();
#ifndef AI_COMPILER
	m_kt_storage = 0;
	m_kt_own_count = 0;
	if (g_kt_chunk.active)
		kt_merge_chunk();
#endif
	m_row_length = iFloor((header().box().max.z - header().box().min.z) / header().cell_size() + EPS_L + 1.5f);
	m_column_length = iFloor((header().box().max.x - header().box().min.x) / header().cell_size() + EPS_L + 1.5f);
	m_access_mask.assign(header().vertex_count(), true);
	unpack_xz(vertex_position(header().box().max), m_max_x, m_max_z);

#ifdef DEBUG
#	ifndef AI_COMPILER
		m_current_level_id		= -1;
		m_current_actual		= false;
		m_current_center		= Fvector().set(flt_max,flt_max,flt_max);
		m_current_radius		= Fvector().set(flt_max,flt_max,flt_max);
#	endif
#endif
}

CLevelGraph::~CLevelGraph()
{
	FS.r_close(m_reader);
#ifndef AI_COMPILER
	xr_free(m_kt_storage);
#endif
}

#ifndef AI_COMPILER
// Seamless world experiment (kt). Builds one AI map out of two: this level's and the chunk's.
//
// The map is a grid of cells in the level's box; a node keeps its cell number and the numbers of its
// four neighbours, and the nodes lie in the file sorted by cell number. So the merged map gets a box
// that holds both levels on this level's grid, every node gets its cell number in that box, the
// chunk's nodes follow this level's with their neighbour numbers moved up, and the nodes that end up
// in adjacent cells across the border are made neighbours.
//
// Kept simple on purpose: the chunk has to lie wholly at greater x than this level, then the joined
// array is still sorted and this level's node numbers do not change (spawn data and the cross table
// refer to them). Any other placement needs the searches by cell to know about ranges.
void CLevelGraph::kt_merge_chunk()
{
	string_path file;
	FS.update_path(file, "$game_levels$", g_kt_chunk.name);
	xr_strcat(file, "\\level.ai");
	if (!FS.exist(file))
	{
		Msg("! [kt chunk] AI map: no file %s", file);
		return;
	}
	IReader* chunk_reader = FS.r_open(file);

	hdrNODES H1, H2;
	CopyMemory(&H1, m_header, sizeof(hdrNODES));
	CopyMemory(&H2, chunk_reader->pointer(), sizeof(hdrNODES));
	const NodeCompressed* N1 = (const NodeCompressed*)(void*)m_nodes;
	const NodeCompressed* N2 = (const NodeCompressed*)((const u8*)chunk_reader->pointer() + sizeof(hdrNODES));
	const float cell = H1.size;
	const u32 invalid = (1 << MAX_NODE_BIT_COUNT) - 1;

	struct local
	{
		static u32 rows(const hdrNODES& H) { return u32(iFloor((H.aabb.max.z - H.aabb.min.z) / H.size + EPS_L + 1.5f)); }
		static Fvector position(const hdrNODES& H, const NodeCompressed& N)
		{
			u32 row = rows(H);
			Fvector P;
			P.x = float(N.p.xz() / row) * H.size + H.aabb.min.x;
			P.z = float(N.p.xz() % row) * H.size + H.aabb.min.z;
			P.y = (float(N.p.y()) / 65535) * H.size_y + H.aabb.min.y;
			return P;
		}
	};

	bool ok = (H2.version == H1.version) && fsimilar(H2.size, cell);
	if (!ok) Msg("! [kt chunk] AI map: version or cell size differ");

	// The box of both, on this level's grid: its low corner is a whole number of cells below ours.
	Fbox box2 = H2.aabb;
	box2.min.add(g_kt_chunk.offset);
	box2.max.add(g_kt_chunk.offset);
	hdrNODES HM = H1;
	HM.aabb.merge(box2);
	HM.aabb.min.x = H1.aabb.min.x - cell * float(iCeil((H1.aabb.min.x - HM.aabb.min.x) / cell));
	HM.aabb.min.z = H1.aabb.min.z - cell * float(iCeil((H1.aabb.min.z - HM.aabb.min.z) / cell));
	HM.aabb.max.x += cell;
	HM.aabb.max.z += cell;
	HM.size_y = HM.aabb.max.y - HM.aabb.min.y + EPS_L;
	HM.count = H1.count + H2.count;
	const u32 row = local::rows(HM);
	const u32 column = u32(iFloor((HM.aabb.max.x - HM.aabb.min.x) / cell + EPS_L + 1.5f));

	if (ok && (HM.count >= invalid || u64(row) * column >= u64(invalid)))
	{
		Msg("! [kt chunk] AI map: both levels do not fit the limits: %u nodes, %u x %u cells", HM.count, column, row);
		ok = false;
	}

	u8* storage = 0;
	NodeCompressed* NM = 0;
	u32 max_cell_own = 0, min_cell_chunk = u32(-1);
	if (ok)
	{
		storage = xr_alloc<u8>(sizeof(hdrNODES) + HM.count * sizeof(NodeCompressed));
		CopyMemory(storage, &HM, sizeof(hdrNODES));
		NM = (NodeCompressed*)(storage + sizeof(hdrNODES));
		for (u32 i = 0; i < HM.count; i++)
		{
			const bool own = i < H1.count;
			NodeCompressed& N = NM[i];
			N = own ? N1[i] : N2[i - H1.count];
			Fvector P = local::position(own ? H1 : H2, N);
			if (!own)
			{
				P.add(g_kt_chunk.offset);
				for (u8 k = 0; k < 4; k++)
					if (N.link(k) != invalid) N.link(k, N.link(k) + H1.count);
			}
			u32 pxz = u32(iFloor((P.x - HM.aabb.min.x) / cell + .5f)) * row + u32(iFloor((P.z - HM.aabb.min.z) / cell + .5f));
			int py = iFloor(65535.f * (P.y - HM.aabb.min.y) / HM.size_y + EPS_S);
			clamp(py, 0, 65535);
			N.p.xz(pxz);
			N.p.y(u16(py));
			if (own) max_cell_own = _max(max_cell_own, pxz);
			else min_cell_chunk = _min(min_cell_chunk, pxz);
		}
		if (min_cell_chunk <= max_cell_own)
		{
			Msg("! [kt chunk] AI map: the chunk must lie wholly at greater x than the level (cells %u and %u)", max_cell_own, min_cell_chunk);
			xr_free(storage);
			ok = false;
		}
	}

	u32 stitched = 0;
	if (ok)
	{
		// Neighbours across the border. Links: 0 is x-1, 1 is z+1, 2 is x+1, 3 is z-1.
		static const int dx[4] = {-1, 0, 1, 0};
		static const int dz[4] = {0, 1, 0, -1};
		NodeCompressed* own_begin = NM;
		NodeCompressed* own_end = NM + H1.count;
		for (u32 i = H1.count; i < HM.count; i++)
		{
			NodeCompressed& N = NM[i];
			for (u8 k = 0; k < 4; k++)
			{
				if (N.link(k) != invalid) continue;
				int x = int(N.p.xz() / row) + dx[k];
				int z = int(N.p.xz() % row) + dz[k];
				if (x < 0 || z < 0 || z >= int(row)) continue;
				u32 wanted = u32(x) * row + u32(z);
				// first own node of that cell
				NodeCompressed* lo = own_begin;
				NodeCompressed* hi = own_end;
				while (lo < hi)
				{
					NodeCompressed* mid = lo + (hi - lo) / 2;
					if (mid->p.xz() < wanted) lo = mid + 1;
					else hi = mid;
				}
				const u8 back = (k + 2) % 4;
				const float y = (float(N.p.y()) / 65535) * HM.size_y;
				for (NodeCompressed* M = lo; M != own_end && M->p.xz() == wanted; ++M)
				{
					if (M->link(back) != invalid) continue;
					if (_abs((float(M->p.y()) / 65535) * HM.size_y - y) > 1.f) continue;
					N.link(k, u32(M - NM));
					M->link(back, i);
					stitched++;
					break;
				}
			}
		}
		m_kt_storage = storage;
		m_kt_own_count = H1.count;
		m_header = (CHeader*)storage;
		m_nodes = (CVertex*)(void*)NM;
		Msg("* [kt chunk] AI map: %u nodes of the level and %u of the chunk in %u x %u cells, %u links across the border",
		    H1.count, H2.count, column, row, stitched);
	}
	FS.r_close(chunk_reader);
}
#endif

u32 CLevelGraph::vertex(const Fvector& position) const
{
	CLevelGraph::CPosition _node_position;
	vertex_position(_node_position, position);
	float min_dist = flt_max;
	u32 selected;
	set_invalid_vertex(selected);
	for (u32 i = 0; i < header().vertex_count(); ++i)
	{
		float dist = distance(i, position);
		if (dist < min_dist)
		{
			min_dist = dist;
			selected = i;
		}
	}

	VERIFY(valid_vertex_id(selected));
	return (selected);
}

u32 CLevelGraph::vertex(u32 current_node_id, const Fvector& position) const
{
	START_PROFILE("Level_Graph::find vertex")
#ifndef AI_COMPILER
		Device.Statistic->AI_Node.Begin();
#endif

		u32 id;

		if (valid_vertex_position(position))
		{
			// so, our position is inside the level graph bounding box
			if (valid_vertex_id(current_node_id) && inside(vertex(current_node_id), position))
			{
				// so, our node corresponds to the position
#ifndef AI_COMPILER
				Device.Statistic->AI_Node.End();
#endif
				return (current_node_id);
			}

			// so, our node doesn't correspond to the position
			// try to search it with O(logN) time algorithm
			u32 _vertex_id = vertex_id(position);
			if (valid_vertex_id(_vertex_id))
			{
				// so, there is a node which corresponds with x and z to the position
				bool ok = true;
				if (valid_vertex_id(current_node_id))
				{
					{
						CVertex const& vertex = *this->vertex(current_node_id);
						for (u32 i = 0; i < 4; ++i)
						{
							if (vertex.link(i) == _vertex_id)
							{
#ifndef AI_COMPILER
								Device.Statistic->AI_Node.End();
#endif // AI_COMPILER
								return (_vertex_id);
							}
						}
					}
					{
						CVertex const& vertex = *this->vertex(_vertex_id);
						for (u32 i = 0; i < 4; ++i)
						{
							if (vertex.link(i) == current_node_id)
							{
#ifndef AI_COMPILER
								Device.Statistic->AI_Node.End();
#endif // AI_COMPILER
								return (_vertex_id);
							}
						}
					}

					float y0 = vertex_plane_y(current_node_id, position.x, position.z);
					float y1 = vertex_plane_y(_vertex_id, position.x, position.z);
					bool over0 = position.y > y0;
					bool over1 = position.y > y1;
					float y_dist0 = position.y - y0;
					float y_dist1 = position.y - y1;
					if (over0)
					{
						if (over1)
						{
							if (y_dist1 - y_dist0 > 1.f)
								ok = false;
							else
								ok = true;
						}
						else
						{
							if (y_dist0 - y_dist1 > 1.f)
								ok = false;
							else
								ok = true;
						}
					}
					else
					{
						ok = true;
					}
				}
				if (ok)
				{
#ifndef AI_COMPILER
					Device.Statistic->AI_Node.End();
#endif
					return (_vertex_id);
				}
			}
		}

		if (!valid_vertex_id(current_node_id))
		{
			// so, we do not have a correct current node
			// performing very slow full search
			id = vertex(position);
			VERIFY(valid_vertex_id(id));
#ifndef AI_COMPILER
			Device.Statistic->AI_Node.End();
#endif
			return (id);
		}

		u32 new_vertex_id = guess_vertex_id(current_node_id, position);
		if (new_vertex_id != current_node_id)
			return (new_vertex_id);

		// so, our position is outside the level graph bounding box
		// or
		// there is no node for the current position
		// try to search the nearest one iteratively
		SContour _contour;
		Fvector point;
		u32 best_vertex_id = current_node_id;
		contour(_contour, current_node_id);
		nearest(point, position, _contour);
		float best_distance_sqr = position.distance_to_sqr(point);
		const_iterator i, e;
		begin(current_node_id, i, e);
		for (; i != e; ++i)
		{
			u32 level_vertex_id = value(current_node_id, i);
			if (!valid_vertex_id(level_vertex_id))
				continue;

			contour(_contour, level_vertex_id);
			nearest(point, position, _contour);
			float distance_sqr = position.distance_to_sqr(point);
			if (best_distance_sqr > distance_sqr)
			{
				best_distance_sqr = distance_sqr;
				best_vertex_id = level_vertex_id;
			}
		}

#ifndef AI_COMPILER
		Device.Statistic->AI_Node.End();
#endif
		return (best_vertex_id);

	STOP_PROFILE
}

u32 CLevelGraph::vertex_id(const Fvector& position) const
{
	VERIFY2(
		valid_vertex_position(position),
		make_string(
			"invalid position for CLevelGraph::vertex_id specified: [%f][%f][%f]",
			VPUSH(position)
		)
	);

	if (!valid_vertex_position(position))
	{
		Msg("{@} --- Invalid position for CLevelGraph::vertex_id specified: [%f][%f][%f]", VPUSH(position));
		return (u32(-1));
	}

	CPosition _vertex_position = vertex_position(position);
	CVertex* B = m_nodes;
	CVertex* E = m_nodes + header().vertex_count();
	CVertex* I = std::lower_bound(B, E, _vertex_position.xz());
	if ((I == E) || ((*I).position().xz() != _vertex_position.xz()))
		return (u32(-1));

	u32 best_vertex_id = u32(I - B);
	float y = vertex_plane_y(best_vertex_id, position.x, position.z);
	for (++I; I != E; ++I)
	{
		if ((*I).position().xz() != _vertex_position.xz())
			break;

		u32 new_vertex_id = u32(I - B);
		float _y = vertex_plane_y(new_vertex_id, position.x, position.z);
		if (y <= position.y)
		{
			// so, current node is under the specified position
			if (_y <= position.y)
			{
				// so, new node is under the specified position
				if (position.y - _y < position.y - y)
				{
					// so, new node is closer to the specified position
					y = _y;
					best_vertex_id = new_vertex_id;
				}
			}
		}
		else
			// so, current node is over the specified position
			if (_y <= position.y)
			{
				// so, new node is under the specified position
				y = _y;
				best_vertex_id = new_vertex_id;
			}
			else
				// so, new node is over the specified position
				if (_y - position.y < y - position.y)
				{
					// so, new node is closer to the specified position
					y = _y;
					best_vertex_id = new_vertex_id;
				}
	}

	return (best_vertex_id);
}

static const int max_guess_vertex_count = 4;

u32 CLevelGraph::guess_vertex_id(u32 const& current_vertex_id, Fvector const& position) const
{
	VERIFY(valid_vertex_id(current_vertex_id));

	CPosition vertex_position;
	if (valid_vertex_position(position))
		vertex_position = this->vertex_position(position);
	else
		vertex_position = vertex(current_vertex_id)->position();

	u32 x, z;
	unpack_xz(vertex_position, x, z);

	SContour vertex_contour;
	contour(vertex_contour, current_vertex_id);
	Fvector best_point;
	float result_distance = nearest(best_point, position, vertex_contour);
	u32 result_vertex_id = current_vertex_id;

	CVertex const* B = m_nodes;
	CVertex const* E = m_nodes + header().vertex_count();
	u32 start_x = (u32)_max(0, int(x) - max_guess_vertex_count);
	u32 stop_x = _min(max_x(), x + (u32)max_guess_vertex_count);
	u32 start_z = (u32)_max(0, int(z) - max_guess_vertex_count);
	u32 stop_z = _min(max_z(), z + (u32)max_guess_vertex_count);
	for (u32 i = start_x; i <= stop_x; ++i)
	{
		for (u32 j = start_z; j <= stop_z; ++j)
		{
			u32 test_xz = i * m_row_length + j;
			CVertex const* I = std::lower_bound(B, E, test_xz);
			if (I == E)
				continue;

			if ((*I).position().xz() != test_xz)
				continue;

			u32 best_vertex_id = u32(I - B);
			contour(vertex_contour, best_vertex_id);
			float best_distance = nearest(best_point, position, vertex_contour);
			for (++I; I != E; ++I)
			{
				if ((*I).position().xz() != test_xz)
					break;

				u32 vertex_id = u32(I - B);
				Fvector point;
				contour(vertex_contour, vertex_id);
				float distance = nearest(point, position, vertex_contour);
				if (distance >= best_distance)
					continue;

				best_point = point;
				best_distance = distance;
				best_vertex_id = vertex_id;
			}

			if (_abs(best_point.y - position.y) >= 3.f)
				continue;

			if (result_distance <= best_distance)
				continue;

			result_distance = best_distance;
			result_vertex_id = best_vertex_id;
		}
	}

	return (result_vertex_id);
}
