#include "stdafx.h"

#include "xr_area.h"
#include "../xrengine/xr_object.h"
#include "../xrengine/xrLevel.h"
#include "../xrengine/xr_collide_form.h"

using namespace collide;

//----------------------------------------------------------------------
// Class	: CObjectSpace
// Purpose	: stores space slots
//----------------------------------------------------------------------
CObjectSpace::CObjectSpace()
#ifdef DEBUG
	,m_pRender(0)
#endif
{
#ifdef DEBUG
	if( RenderFactory )	
		m_pRender = CNEW(FactoryPtr<IObjectSpaceRender>)() ;

	//sh_debug.create				("debug\\wireframe","$null");
#endif
	m_BoundingVolume.invalidate();
}

//----------------------------------------------------------------------
CObjectSpace::~CObjectSpace()
{
	//moved to ~IGameLevel
	//	Sound->set_geometry_occ		(NULL);
	//	Sound->set_handler			(NULL);
	//
#ifdef DEBUG
	//sh_debug.destroy			();
	CDELETE(m_pRender);
#endif
}

//----------------------------------------------------------------------

//----------------------------------------------------------------------
int CObjectSpace::GetNearest(xr_vector<ISpatialShared>& q_spatial, xr_vector<CObject*>& q_nearest, const Fvector& point, float range, CObject* ignore_object)
{
	q_spatial.clear_not_free();
	// Query objects
	q_nearest.clear_not_free();
	Fsphere Q;
	Q.set(point, range);
	Fvector B;
	B.set(range, range, range);
	g_SpatialSpace->q_box(q_spatial, 0, STYPE_COLLIDEABLE, point, B);

	// Iterate
	auto it = q_spatial.begin();
	auto end = q_spatial.end();
	for (; it != end; it++)
	{
		CObject* O = (*it)->dcast_CObject();
		if (0 == O)
			continue;

		if (O == ignore_object)
			continue;

		Fsphere mS = { O->SpatialComponent->spatial.sphere.P, O->SpatialComponent->spatial.sphere.R };
		if (Q.intersect(mS))
			q_nearest.push_back(O);
	}

	return (int)q_nearest.size();
}

//----------------------------------------------------------------------
int CObjectSpace::GetNearest(xr_vector<CObject*>& q_nearest, ICollisionForm* obj, float range)
{
	CObject* O = obj->Owner();
	return GetNearest(q_nearest, O->SpatialComponent->spatial.sphere.P, range + O->SpatialComponent->spatial.sphere.R, O);
}

//----------------------------------------------------------------------


void CObjectSpace::Load(CDB::build_callback build_callback)
{
	Load("$level$", "level.cform", build_callback);
}

void CObjectSpace::Load(LPCSTR path, LPCSTR fname, CDB::build_callback build_callback)
{
#ifdef USE_ARENA_ALLOCATOR
	Msg( "CObjectSpace::Load, g_collision_allocator.get_allocated_size() - %d", int(g_collision_allocator.get_allocated_size()/1024.0/1024) );
#endif // #ifdef USE_ARENA_ALLOCATOR
	IReader* F = FS.r_open(path, fname);
	R_ASSERT(F);
	Load(F, build_callback);
}

void CObjectSpace::Load(IReader* F, CDB::build_callback build_callback)
{
	hdrCFORM H;
	F->r(&H, sizeof(hdrCFORM));
	R_ASSERT(CFORM_CURRENT_VERSION == H.version);
	m_BoundingVolume.set(H.aabb);

	g_SpatialSpace->initialize(m_BoundingVolume);
	g_SpatialSpacePhysic->initialize(m_BoundingVolume);
	g_SpatialSpaceLights->initialize(m_BoundingVolume);

	// Copy the geometry out of the reader on this thread, so GetStaticVerts() and
	// GetStaticTris() return usable arrays before Load returns. The reader can then
	// be closed here instead of from the task.
	Fvector* verts = (Fvector*)F->pointer();
	CDB::TRI* tris = (CDB::TRI*)(verts + H.vertcount);
	Static.build_arrays(verts, H.vertcount, tris, H.facecount, build_callback);
	FS.r_close(F);

	// Only the OPCODE tree build goes to the background. It is the expensive half,
	// so the overlap with the rest of the level load is kept. The four syncronize()
	// calls in the CDB query paths already cover the tree.
	const DWORD this_thread_id = GetCurrentThreadId();
	Static.async_cform_load.run([this, this_thread_id]()
	{
		if (this_thread_id != GetCurrentThreadId()) { PROF_THREAD("X-Ray PPL Thread") }
		PROF_EVENT("Async cform tree build");
		Static.build_tree();
	});
}

typedef bool (*kt_cut_function)(u32 piece, u32 material, const Fvector& a, const Fvector& b, const Fvector& c);

// One set of arrays out of the level's collision file and the pieces': see LoadWithChunks.
static void kt_gather(bool base, xr_vector<KtCform>& chunks, kt_cut_function cut, Fvector*& all_verts, u32& vert_count, CDB::TRI*& all_tris, u32& tri_count, Fbox& volume)
{
	IReader* F = FS.r_open("$level$", "level.cform");
	R_ASSERT(F);
	hdrCFORM H;
	F->r(&H, sizeof(hdrCFORM));
	R_ASSERT(CFORM_CURRENT_VERSION == H.version);
	Fvector* verts = (Fvector*)F->pointer();
	CDB::TRI* tris = (CDB::TRI*)(verts + H.vertcount);

	xr_vector<IReader*> readers(chunks.size());
	xr_vector<hdrCFORM> headers(chunks.size());
	// `base` off: the level's own triangles are left out (it is let go on the move, the pieces stay)
	if (!base) H.vertcount = H.facecount = 0;
	vert_count = H.vertcount;
	tri_count = H.facecount;
	volume.set(H.aabb);
	for (u32 c = 0; c < chunks.size(); c++)
	{
		readers[c] = FS.r_open(chunks[c].file);
		R_ASSERT2(readers[c], chunks[c].file);
		readers[c]->r(&headers[c], sizeof(hdrCFORM));
		R_ASSERT(CFORM_CURRENT_VERSION == headers[c].version);
		vert_count += headers[c].vertcount;
		tri_count += headers[c].facecount;
		Fbox box = headers[c].aabb;
		box.min.add(chunks[c].offset);
		box.max.add(chunks[c].offset);
		volume.merge(box);
	}

	// One model for all: the vertices of a chunk are shifted, its triangles point at them.
	// A triangle the experiment cuts out (a doorway, another level's land) is left out.
	all_verts = xr_alloc<Fvector>(vert_count);
	all_tris = xr_alloc<CDB::TRI>(tri_count);
	CopyMemory(all_verts, verts, H.vertcount * sizeof(Fvector));
	u32 kept = 0;
	for (u32 i = 0; i < H.facecount; i++)
		if (!cut(0, tris[i].material, all_verts[tris[i].verts[0]], all_verts[tris[i].verts[1]], all_verts[tris[i].verts[2]])) all_tris[kept++] = tris[i];
	u32 dropped = H.facecount - kept;
	u32 vert_base = H.vertcount;
	for (u32 c = 0; c < chunks.size(); c++)
	{
		Fvector* verts2 = (Fvector*)readers[c]->pointer();
		CDB::TRI* tris2 = (CDB::TRI*)(verts2 + headers[c].vertcount);
		for (u32 i = 0; i < headers[c].vertcount; i++)
			all_verts[vert_base + i].add(verts2[i], chunks[c].offset);
		chunks[c].tri_base = kept;
		for (u32 i = 0; i < headers[c].facecount; i++)
		{
			CDB::TRI T = tris2[i];
			T.verts[0] += vert_base;
			T.verts[1] += vert_base;
			T.verts[2] += vert_base;
			if (!cut(chunks[c].piece + 1, T.material, all_verts[T.verts[0]], all_verts[T.verts[1]], all_verts[T.verts[2]])) all_tris[kept++] = T;
			else dropped++;
		}
		chunks[c].tri_count = kept - chunks[c].tri_base;
		vert_base += headers[c].vertcount;
	}
	if (dropped) Msg("* [kt chunk] collision: %u triangles are cut out", dropped);
	tri_count = kept;

	FS.r_close(F);
	for (u32 c = 0; c < chunks.size(); c++) FS.r_close(readers[c]);
}

void CObjectSpace::LoadWithChunks(CDB::build_callback build_callback, xr_vector<KtCform>& chunks, bool (*cut)(u32 piece, u32 material, const Fvector& a, const Fvector& b, const Fvector& c))
{
	Fvector* all_verts;
	CDB::TRI* all_tris;
	u32 vert_count, tri_count;
	kt_gather(true, chunks, cut, all_verts, vert_count, all_tris, tri_count, m_BoundingVolume);

	g_SpatialSpace->initialize(m_BoundingVolume);
	g_SpatialSpacePhysic->initialize(m_BoundingVolume);
	g_SpatialSpaceLights->initialize(m_BoundingVolume);

	Static.build_arrays(all_verts, vert_count, all_tris, tri_count, build_callback);
	xr_free(all_verts);
	xr_free(all_tris);

	const DWORD this_thread_id = GetCurrentThreadId();
	Static.async_cform_load.run([this, this_thread_id]()
	{
		if (this_thread_id != GetCurrentThreadId()) { PROF_THREAD("X-Ray PPL Thread") }
		PROF_EVENT("Async cform tree build");
		Static.build_tree();
	});
}

// Seamless world experiment (kt): the same model for another set of pieces, built whole into
// `target` (any thread); the level's own model is not touched until kt_SwapStatic.
void CObjectSpace::kt_BuildModel(CDB::MODEL& target, bool base, CDB::build_callback build_callback, xr_vector<KtCform>& chunks, bool (*cut)(u32 piece, u32 material, const Fvector& a, const Fvector& b, const Fvector& c))
{
	Fvector* all_verts;
	CDB::TRI* all_tris;
	u32 vert_count, tri_count;
	Fbox volume;
	kt_gather(base, chunks, cut, all_verts, vert_count, all_tris, tri_count, volume);
	target.build_arrays(all_verts, vert_count, all_tris, tri_count, build_callback);
	xr_free(all_verts);
	xr_free(all_tris);
	target.build_tree();
}

XRCDB_API u32 g_kt_static_generation = 0;

void CObjectSpace::kt_SwapStatic(CDB::MODEL& fresh)
{
	Static.kt_swap(fresh);
	g_kt_static_generation++;
}

void CObjectSpace::Create(Fvector* verts, CDB::TRI* tris, const hdrCFORM& H, CDB::build_callback build_callback, bool init_bounds)
{
	Static.build(verts, H.vertcount, tris, H.facecount, build_callback);

	if (init_bounds)
	{
		m_BoundingVolume.set(H.aabb);

		g_SpatialSpace->initialize(m_BoundingVolume);
		g_SpatialSpacePhysic->initialize(m_BoundingVolume);
		g_SpatialSpaceLights->initialize(m_BoundingVolume);
	}
}

//----------------------------------------------------------------------
#ifdef DEBUG
void CObjectSpace::dbgRender()
{
	(*m_pRender)->dbgRender();
}
/*
void CObjectSpace::dbgRender()
{
	R_ASSERT(bDebug);

	RCache.set_Shader(sh_debug);
	for (u32 i=0; i<q_debug.boxes.size(); i++)
	{
		Fobb&		obb		= q_debug.boxes[i];
		Fmatrix		X,S,R;
		obb.xform_get(X);
		RCache.dbg_DrawOBB(X,obb.m_halfsize,D3DCOLOR_XRGB(255,0,0));
		S.scale		(obb.m_halfsize);
		R.mul		(X,S);
		RCache.dbg_DrawEllipse(R,D3DCOLOR_XRGB(0,0,255));
	}
	q_debug.boxes.clear();

	for (i=0; i<dbg_S.size(); i++)
	{
		std::pair<Fsphere,u32>& P = dbg_S[i];
		Fsphere&	S = P.first;
		Fmatrix		M;
		M.scale		(S.R,S.R,S.R);
		M.translate_over(S.P);
		RCache.dbg_DrawEllipse(M,P.second);
	}
	dbg_S.clear();
}
*/
#endif
