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
#include <queue>
#include <functional>
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
	if (kt_chunk_wanted())
		kt_merge_chunk();
#endif
	m_row_length = iFloor((header().box().max.z - header().box().min.z) / header().cell_size() + EPS_L + 1.5f);
	m_column_length = iFloor((header().box().max.x - header().box().min.x) / header().cell_size() + EPS_L + 1.5f);
	m_access_mask.assign(header().vertex_count(), true);
	unpack_xz(vertex_position(header().box().max), m_max_x, m_max_z);
#ifndef AI_COMPILER
	if (m_kt_own_count) kt_build_reach(); // after the row length is known: positions of nodes are asked
#endif

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
// Seamless world experiment (kt). Builds one AI map out of several: this level's, the chunk's and
// the small pieces that fill the passage between them (-kt_ai_piece).
//
// The map is a grid of cells in the level's box; a node keeps its cell number and the numbers of its
// four neighbours, and the nodes lie in the file sorted by cell number. So the merged map gets a box
// that holds everything on this level's grid and every node gets its cell number in that box. This
// level's nodes stay first and keep their numbers (spawn data and the cross table refer to them);
// all the added nodes follow as a second run sorted by cell number among themselves. Nodes of
// different origin that end up in adjacent cells with free sides facing each other are made neighbours.
//
// The array is therefore two sorted runs, not one: the searches by cell look into both (kt_runs).
// Seamless world experiment (kt): from where each level is a short walk. For every piece the map
// is flooded from its border outwards, through the nodes of the others, as far as -kt_reach looks;
// the rule that holds levels loaded then asks by the player's node. The way is counted over the
// nodes, corners cut where both sides are open.
void CLevelGraph::kt_build_reach()
{
	const u32 count = header().vertex_count();
	m_kt_piece.assign(count, u8(0));
	for (u32 n = m_kt_own_count; n < count; n++)
	{
		u32 chunk, local;
		m_kt_piece[n] = kt_local(n, chunk, local) ? u8(chunk + 1) : u8(255);
	}
	m_kt_reach.clear();
	const float limit = g_kt_chunk.reach_out;
	if (limit <= 0.f) return;

	CTimer timer;
	timer.Start();
	const u32 pieces = 1 + u32(g_kt_chunk.pieces.size());
	m_kt_reach.resize(pieces);
	const float cell = header().cell_size(), diagonal = cell * 1.41421356f;
	// a link is to the next cell; the game's own maps have a stray one that leads far away
	auto next_to = [&](u32 a, u32 b)
	{
		const Fvector A = vertex_position(a), B = vertex_position(b);
		return _abs(A.x - B.x) + _abs(A.z - B.z) < cell * 2.5f;
	};
	typedef std::pair<float, u32> Item;
	xr_vector<float> metres(count);
	string256 report = "";
	for (u32 p = 0; p < pieces; p++)
	{
		std::fill(metres.begin(), metres.end(), flt_max);
		std::priority_queue<Item, xr_vector<Item>, std::greater<Item> > queue;
		for (u32 n = 0; n < count; n++)
		{
			if (m_kt_piece[n] == p) continue;
			const CVertex* V = vertex(n);
			for (int k = 0; k < 4; k++)
			{
				const u32 to = V->link(k);
				if (to < count && m_kt_piece[to] == p && next_to(n, to))
				{
					metres[n] = cell;
					queue.push(Item(cell, n));
					break;
				}
			}
		}
		xr_vector<std::pair<u32, float> >& table = m_kt_reach[p];
		while (!queue.empty())
		{
			const Item top = queue.top();
			queue.pop();
			if (top.first > metres[top.second]) continue;
			table.push_back(std::make_pair(top.second, top.first));
			const CVertex* V = vertex(top.second);
			u32 side[4];
			for (int k = 0; k < 4; k++)
			{
				side[k] = V->link(k);
				if (side[k] >= count || m_kt_piece[side[k]] == p || !next_to(top.second, side[k])) side[k] = u32(-1);
			}
			for (int k = 0; k < 4; k++)
			{
				if (side[k] == u32(-1)) continue;
				float way = top.first + cell;
				if (way <= limit && way < metres[side[k]])
				{
					metres[side[k]] = way;
					queue.push(Item(way, side[k]));
				}
				if (side[(k + 1) & 3] == u32(-1)) continue;
				const u32 corner = vertex(side[k])->link((k + 1) & 3);
				if (corner >= count || m_kt_piece[corner] == p || !next_to(side[k], corner)) continue;
				way = top.first + diagonal;
				if (way <= limit && way < metres[corner])
				{
					metres[corner] = way;
					queue.push(Item(way, corner));
				}
			}
		}
		std::sort(table.begin(), table.end());
		string64 one;
		xr_sprintf(one, " %u", u32(table.size()));
		xr_strcat(report, one);
	}
	Msg("* [kt stream] reach: nodes within %.0f m of walking from the level and from each attached piece:%s; counted in %u ms", limit, report, timer.GetElapsed_ms());
}

float CLevelGraph::kt_reach(u32 piece, u32 node) const
{
	if (node >= m_kt_piece.size() || piece >= m_kt_reach.size()) return flt_max;
	if (m_kt_piece[node] == piece) return 0.f;
	const xr_vector<std::pair<u32, float> >& table = m_kt_reach[piece];
	xr_vector<std::pair<u32, float> >::const_iterator I = std::lower_bound(table.begin(), table.end(), std::make_pair(node, 0.f));
	return (I != table.end() && I->first == node) ? I->second : flt_max;
}

void CLevelGraph::kt_merge_chunk()
{
	struct Piece
	{
		IReader* reader;
		hdrNODES H;
		const NodeCompressed* N;
		Fvector offset;
		int chunk;
	};
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

	CTimer kt_timer;
	kt_timer.Start();
	hdrNODES H1;
	CopyMemory(&H1, m_header, sizeof(hdrNODES));
	const NodeCompressed* N1 = (const NodeCompressed*)(void*)m_nodes;
	const float cell = H1.size;
	const u32 invalid = (1 << MAX_NODE_BIT_COUNT) - 1;

	bool ok = true;
	xr_vector<Piece> pieces;
	const u32 chunk_count = u32(g_kt_chunk.pieces.size());
	for (u32 n = 0; n < chunk_count + g_kt_chunk.ai_pieces.size(); n++)
	{
		string_path file;
		Piece piece;
		if (n < chunk_count)
		{
			// a level standing next to this one; a piece of plain geometry may have no AI map
			FS.update_path(file, "$game_levels$", g_kt_chunk.pieces[n].name);
			xr_strcat(file, "\\level.ai");
			piece.offset = g_kt_chunk.pieces[n].offset;
			piece.chunk = int(n);
		}
		else
		{
			FS.update_path(file, "$game_levels$", *g_kt_chunk.ai_pieces[n - chunk_count]);
			piece.offset.set(0, 0, 0);
			piece.chunk = -1;
		}
		if (!FS.exist(file))
		{
			Msg("%s [kt chunk] AI map: no file %s", n < chunk_count ? "*" : "!", file);
			continue;
		}
		piece.reader = FS.r_open(file);
		CopyMemory(&piece.H, piece.reader->pointer(), sizeof(hdrNODES));
		piece.N = (const NodeCompressed*)((const u8*)piece.reader->pointer() + sizeof(hdrNODES));
		if (piece.H.version != H1.version || !fsimilar(piece.H.size, cell))
		{
			Msg("! [kt chunk] AI map: version or cell size differ in %s", file);
			ok = false;
		}
		pieces.push_back(piece);
	}

	// The box of all, on this level's grid: its low corner is a whole number of cells below ours.
	hdrNODES HM = H1;
	u32 extra = 0;
	for (u32 n = 0; n < pieces.size(); n++)
	{
		Fbox box = pieces[n].H.aabb;
		box.min.add(pieces[n].offset);
		box.max.add(pieces[n].offset);
		HM.aabb.merge(box);
		extra += pieces[n].H.count;
	}
	HM.aabb.min.x = H1.aabb.min.x - cell * float(iCeil((H1.aabb.min.x - HM.aabb.min.x) / cell));
	HM.aabb.min.z = H1.aabb.min.z - cell * float(iCeil((H1.aabb.min.z - HM.aabb.min.z) / cell));
	HM.aabb.max.x += cell;
	HM.aabb.max.z += cell;
	HM.size_y = HM.aabb.max.y - HM.aabb.min.y + EPS_L;
	HM.count = H1.count + extra;
	const u32 row = local::rows(HM);
	const u32 column = u32(iFloor((HM.aabb.max.x - HM.aabb.min.x) / cell + EPS_L + 1.5f));

	if (ok && (HM.count >= invalid || u64(row) * column >= u64(KT_MAX_CELL)))
	{
		Msg("! [kt chunk] AI map: the levels do not fit the limits together: %u nodes, %u x %u cells", HM.count, column, row);
		ok = false;
	}

	u32 stitched = 0;
	if (ok)
	{
		u8* storage = xr_alloc<u8>(sizeof(hdrNODES) + HM.count * sizeof(NodeCompressed));
		CopyMemory(storage, &HM, sizeof(hdrNODES));
		NodeCompressed* NM = (NodeCompressed*)(storage + sizeof(hdrNODES));

		struct repack
		{
			static void position(NodeCompressed& N, const Fvector& P, const hdrNODES& HM, u32 row)
			{
				u32 pxz = u32(iFloor((P.x - HM.aabb.min.x) / HM.size + .5f)) * row + u32(iFloor((P.z - HM.aabb.min.z) / HM.size + .5f));
				int py = iFloor(65535.f * (P.y - HM.aabb.min.y) / HM.size_y + EPS_S);
				clamp(py, 0, 65535);
				N.p.xz(pxz);
				N.p.y(u16(py));
			}
		};

		// this level's nodes: same numbers, new cell numbers
		for (u32 i = 0; i < H1.count; i++)
		{
			NM[i] = N1[i];
			repack::position(NM[i], local::position(H1, N1[i]), HM, row);
		}

		// the added nodes, in the order of their files; neighbours numbered within the added ones
		xr_vector<NodeCompressed> added(extra);
		xr_vector<u8> origin(extra);
		u32 base = 0;
		for (u32 n = 0; n < pieces.size(); n++)
		{
			const Piece& piece = pieces[n];
			for (u32 i = 0; i < piece.H.count; i++)
			{
				NodeCompressed& N = added[base + i];
				N = piece.N[i];
				Fvector P = local::position(piece.H, N);
				P.add(piece.offset);
				repack::position(N, P, HM, row);
				for (u8 k = 0; k < 4; k++)
					if (N.link(k) != invalid) N.link(k, N.link(k) + base);
				origin[base + i] = u8(n + 1);
			}
			base += piece.H.count;
		}

		// sorted by cell number; the order inside a file is kept
		xr_vector<u32> order(extra), place(extra);
		for (u32 i = 0; i < extra; i++) order[i] = i;
		const NodeCompressed* added_begin = extra ? &added[0] : 0;
		std::stable_sort(order.begin(), order.end(), [added_begin](u32 a, u32 b) { return added_begin[a].p.xz() < added_begin[b].p.xz(); });
		for (u32 i = 0; i < extra; i++) place[order[i]] = i;
		xr_vector<u8> origin_sorted(extra);
		for (u32 i = 0; i < extra; i++)
		{
			NodeCompressed& N = NM[H1.count + i];
			N = added[order[i]];
			origin_sorted[i] = origin[order[i]];
			for (u8 k = 0; k < 4; k++)
				if (N.link(k) != invalid) N.link(k, H1.count + place[N.link(k)]);
		}

		// Trimmed tails (-kt_trim): the nodes beyond the line of a joint lose every link, and are not
		// offered to the stitching below. They keep their numbers: the spawn data refers to them.
		xr_vector<u8> dead(HM.count, 0);
		u32 trimmed = 0;
		if (!g_kt_chunk.trims.empty())
		{
			for (u32 i = 0; i < HM.count; i++)
			{
				u32 number = 0;
				if (i >= H1.count)
				{
					const int chunk = pieces[origin_sorted[i - H1.count] - 1].chunk;
					if (chunk < 0) continue;
					number = u32(chunk) + 1;
				}
				const float x = float(NM[i].p.xz() / row) * cell + HM.aabb.min.x;
				const float z = float(NM[i].p.xz() % row) * cell + HM.aabb.min.z;
				if (!g_kt_chunk.trimmed(number, x, z)) continue;
				dead[i] = 1;
				trimmed++;
			}
			for (u32 i = 0; i < HM.count; i++)
				for (u8 k = 0; k < 4; k++)
				{
					const u32 to = NM[i].link(k);
					if (to == invalid) continue;
					if (dead[i] || dead[to]) NM[i].link(k, invalid);
				}
			Msg("* [kt chunk] AI map: %u nodes beyond the lines of the joints are taken out", trimmed);
		}

		// Neighbours across the borders. Links: 0 is x-1, 1 is z+1, 2 is x+1, 3 is z-1.
		static const int dx[4] = {-1, 0, 1, 0};
		static const int dz[4] = {0, 1, 0, -1};
		NodeCompressed* run_begin[2] = {NM, NM + H1.count};
		NodeCompressed* run_end[2] = {NM + H1.count, NM + HM.count};
		for (u32 i = H1.count; i < HM.count; i++)
		{
			NodeCompressed& N = NM[i];
			if (dead[i]) continue;
			const u8 mine = origin_sorted[i - H1.count];
			for (u8 k = 0; k < 4; k++)
			{
				if (N.link(k) != invalid) continue;
				int x = int(N.p.xz() / row) + dx[k];
				int z = int(N.p.xz() % row) + dz[k];
				if (x < 0 || z < 0 || z >= int(row)) continue;
				u32 wanted = u32(x) * row + u32(z);
				const u8 back = (k + 2) % 4;
				const float y = (float(N.p.y()) / 65535) * HM.size_y;
				bool done = false;
				for (u32 r = 0; r < 2 && !done; r++)
				{
					NodeCompressed* lo = run_begin[r];
					NodeCompressed* hi = run_end[r];
					while (lo < hi)
					{
						NodeCompressed* mid = lo + (hi - lo) / 2;
						if (mid->p.xz() < wanted) lo = mid + 1;
						else hi = mid;
					}
					for (NodeCompressed* M = lo; M != run_end[r] && M->p.xz() == wanted; ++M)
					{
						const u8 theirs = (r == 0) ? u8(0) : origin_sorted[u32(M - NM) - H1.count];
						if (theirs == mine) continue;
						if (dead[u32(M - NM)]) continue;
						if (M->link(back) != invalid) continue;
						if (_abs((float(M->p.y()) / 65535) * HM.size_y - y) > 1.f) continue;
						N.link(k, u32(M - NM));
						M->link(back, i);
						stitched++;
						done = true;
						break;
					}
				}
			}
		}
		// Which of this level's nodes every added node hangs on: the one it is reached from over the
		// links. The added node then belongs to the same point of the world graph, and whoever walks
		// to it goes towards the seam it lies behind, not towards one fixed corner of the level.
		m_kt_like.assign(extra, H1.count - 1);
		{
			xr_vector<u32> front, next;
			xr_vector<u8> reached(extra, 0);
			for (u32 i = 0; i < H1.count; i++)
				for (u8 k = 0; k < 4; k++)
				{
					u32 to = NM[i].link(k);
					if (to == invalid || to < H1.count || reached[to - H1.count]) continue;
					reached[to - H1.count] = 1;
					m_kt_like[to - H1.count] = i;
					front.push_back(to);
				}
			while (!front.empty())
			{
				next.clear();
				for (u32 n = 0; n < front.size(); n++)
				{
					const u32 from = front[n];
					for (u8 k = 0; k < 4; k++)
					{
						u32 to = NM[from].link(k);
						if (to == invalid || to < H1.count || reached[to - H1.count]) continue;
						reached[to - H1.count] = 1;
						m_kt_like[to - H1.count] = m_kt_like[from - H1.count];
						next.push_back(to);
					}
				}
				front.swap(next);
			}
		}
		m_kt_ranges.clear();
		for (u32 n = 0, at = 0; n < pieces.size(); at += pieces[n].H.count, n++)
		{
			KtRange range = {pieces[n].chunk, at, pieces[n].H.count};
			m_kt_ranges.push_back(range);
		}
		m_kt_place.swap(place);
		m_kt_order.swap(order);
		m_kt_storage = storage;
		m_kt_own_count = H1.count;
		m_header = (CHeader*)storage;
		m_nodes = (CVertex*)(void*)NM;
		Msg("* [kt chunk] AI map: %u nodes of the level and %u added (%u files) in %u x %u cells, %u links across the borders",
		    H1.count, extra, u32(pieces.size()), column, row, stitched);
		Msg("* [kt time] AI map merged: %u ms", kt_timer.GetElapsed_ms());
	}
	for (u32 n = 0; n < pieces.size(); n++) FS.r_close(pieces[n].reader);
}

#endif

// The sorted runs of the node array: one, or two when a chunk was merged in.
u32 CLevelGraph::kt_runs(CVertex* begin[2], CVertex* end[2]) const
{
	begin[0] = m_nodes;
	end[1] = m_nodes + header().vertex_count();
#ifndef AI_COMPILER
	if (m_kt_own_count)
	{
		end[0] = begin[1] = m_nodes + m_kt_own_count;
		return 2;
	}
#endif
	end[0] = end[1];
	begin[1] = end[1];
	return 1;
}

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

#ifndef AI_COMPILER
// kt: the node has at least one neighbour (the merge leaves the trimmed nodes with none)
static bool kt_linked(const CLevelGraph& graph, u32 vertex_id)
{
	const CLevelGraph::CVertex* V = graph.vertex(vertex_id);
	for (int i = 0; i < 4; i++)
		if (graph.valid_vertex_id(V->link(i)))
			return (true);
	return (false);
}
#endif

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
	// kt: the nodes of the cell are looked for in every sorted run of the array
	CVertex *run_begin[2], *run_end[2];
	const u32 runs = kt_runs(run_begin, run_end);
	u32 best_vertex_id = u32(-1);
	float y = 0;
	for (u32 r = 0; r < runs; r++)
	{
		CVertex* E = run_end[r];
		for (CVertex* I = std::lower_bound(run_begin[r], E, _vertex_position.xz()); I != E; ++I)
		{
			if ((*I).position().xz() != _vertex_position.xz())
				break;

			u32 new_vertex_id = u32(I - B);
			float _y = vertex_plane_y(new_vertex_id, position.x, position.z);
			if (best_vertex_id == u32(-1))
			{
				best_vertex_id = new_vertex_id;
				y = _y;
				continue;
			}
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
	}

#ifndef AI_COMPILER
	// Seamless world experiment (kt), in the merged map only: a node with no link at all is no place to
	// stand. Behind a level's trim lie nodes the merge left unlinked, a few centimetres over the living
	// ones of the level next door in the same cells (68 at the Garbage's joint, 116 in the south
	// entrance's hut); the nearest under a man was often the dead one, and a squad's commander standing
	// on it had no path and ran on the spot (online escape-garbage, 2026-10-20). Of the nodes of the cell
	// within half a metre of the chosen one in height, one with a link is taken instead.
	if (m_kt_own_count && best_vertex_id != u32(-1) && !kt_linked(*this, best_vertex_id))
	{
		u32 linked = u32(-1);
		float linked_dy = .5f;
		for (u32 r = 0; r < runs; r++)
		{
			CVertex* E = run_end[r];
			for (CVertex* I = std::lower_bound(run_begin[r], E, _vertex_position.xz()); I != E; ++I)
			{
				if ((*I).position().xz() != _vertex_position.xz())
					break;
				const u32 id = u32(I - B);
				if (!kt_linked(*this, id))
					continue;
				const float dy = _abs(vertex_plane_y(id, position.x, position.z) - y);
				if (dy <= linked_dy)
				{
					linked_dy = dy;
					linked = id;
				}
			}
		}
		if (linked != u32(-1))
			best_vertex_id = linked;
	}
#endif

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
	// kt: every sorted run of the array is searched
	CVertex *run_begin[2], *run_end[2];
	const u32 runs = kt_runs(run_begin, run_end);
	u32 start_x = (u32)_max(0, int(x) - max_guess_vertex_count);
	u32 stop_x = _min(max_x(), x + (u32)max_guess_vertex_count);
	u32 start_z = (u32)_max(0, int(z) - max_guess_vertex_count);
	u32 stop_z = _min(max_z(), z + (u32)max_guess_vertex_count);
	for (u32 i = start_x; i <= stop_x; ++i)
	{
		for (u32 j = start_z; j <= stop_z; ++j)
		{
			u32 test_xz = i * m_row_length + j;
			for (u32 r = 0; r < runs; r++)
			{
			CVertex const* E = run_end[r];
			CVertex const* I = std::lower_bound((CVertex const*)run_begin[r], E, test_xz);
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
	}

	return (result_vertex_id);
}
