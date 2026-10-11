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

void CObjectSpace::LoadWithChunk(CDB::build_callback build_callback, LPCSTR chunk_cform, const Fvector& offset, const xr_vector<Fbox>& cuts, u32& chunk_tri_base, u32& chunk_tri_count)
{
	IReader* F = FS.r_open("$level$", "level.cform");
	R_ASSERT(F);
	IReader* F2 = FS.r_open(chunk_cform);
	R_ASSERT2(F2, chunk_cform);

	hdrCFORM H, H2;
	F->r(&H, sizeof(hdrCFORM));
	F2->r(&H2, sizeof(hdrCFORM));
	R_ASSERT(CFORM_CURRENT_VERSION == H.version);
	R_ASSERT(CFORM_CURRENT_VERSION == H2.version);

	Fvector* verts = (Fvector*)F->pointer();
	CDB::TRI* tris = (CDB::TRI*)(verts + H.vertcount);
	Fvector* verts2 = (Fvector*)F2->pointer();
	CDB::TRI* tris2 = (CDB::TRI*)(verts2 + H2.vertcount);

	// One model for both: the vertices of the chunk are shifted, its triangles point at them.
	u32 vert_count = H.vertcount + H2.vertcount;
	u32 tri_count = H.facecount + H2.facecount;
	Fvector* all_verts = xr_alloc<Fvector>(vert_count);
	CDB::TRI* all_tris = xr_alloc<CDB::TRI>(tri_count);
	CopyMemory(all_verts, verts, H.vertcount * sizeof(Fvector));
	for (u32 i = 0; i < H2.vertcount; i++)
		all_verts[H.vertcount + i].add(verts2[i], offset);
	// A triangle wholly inside a cut box is left out: a doorway where a wall closed the level.
	struct local
	{
		static bool cut(const xr_vector<Fbox>& cuts, const Fvector* V, const CDB::TRI& T)
		{
			for (u32 c = 0; c < cuts.size(); c++)
				if (cuts[c].contains(V[T.verts[0]]) && cuts[c].contains(V[T.verts[1]]) && cuts[c].contains(V[T.verts[2]])) return true;
			return false;
		}
	};
	u32 own_count = 0;
	for (u32 i = 0; i < H.facecount; i++)
		if (!local::cut(cuts, all_verts, tris[i])) all_tris[own_count++] = tris[i];
	u32 chunk_count = 0;
	for (u32 i = 0; i < H2.facecount; i++)
	{
		CDB::TRI T = tris2[i];
		T.verts[0] += H.vertcount;
		T.verts[1] += H.vertcount;
		T.verts[2] += H.vertcount;
		if (!local::cut(cuts, all_verts, T)) all_tris[own_count + chunk_count++] = T;
	}
	if (tri_count != own_count + chunk_count)
		Msg("* [kt chunk] collision: %u triangles of the level and %u of the chunk are cut out", H.facecount - own_count, H2.facecount - chunk_count);
	tri_count = own_count + chunk_count;
	chunk_tri_base = own_count;
	chunk_tri_count = chunk_count;

	Fbox box2 = H2.aabb;
	box2.min.add(offset);
	box2.max.add(offset);
	m_BoundingVolume.set(H.aabb);
	m_BoundingVolume.merge(box2);

	g_SpatialSpace->initialize(m_BoundingVolume);
	g_SpatialSpacePhysic->initialize(m_BoundingVolume);
	g_SpatialSpaceLights->initialize(m_BoundingVolume);

	Static.build_arrays(all_verts, vert_count, all_tris, tri_count, build_callback);
	xr_free(all_verts);
	xr_free(all_tris);
	FS.r_close(F);
	FS.r_close(F2);

	const DWORD this_thread_id = GetCurrentThreadId();
	Static.async_cform_load.run([this, this_thread_id]()
	{
		if (this_thread_id != GetCurrentThreadId()) { PROF_THREAD("X-Ray PPL Thread") }
		PROF_EVENT("Async cform tree build");
		Static.build_tree();
	});
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
