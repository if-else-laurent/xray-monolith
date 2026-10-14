#include "stdafx.h"
#include "r4.h"
#include "../xrRender/ResourceManager.h"
#include "../xrRender/fbasicvisual.h"
#include "../../xrEngine/fmesh.h"
#include "../../xrEngine/xrLevel.h"
#include "../../xrEngine/x_ray.h"
#include "../../xrEngine/IGame_Persistent.h"
#include "../../xrCore/stream_reader.h"

#include "../xrRender/dxRenderDeviceRender.h"

#include "../xrRenderDX10/dx10BufferUtils.h"
#include "../xrRenderDX10/3DFluid/dx103DFluidVolume.h"

#include "../xrRender/FHierrarhyVisual.h"

#pragma warning(push)
#pragma warning(disable:4995)
#include <malloc.h>
#pragma warning(pop)

void CRender::level_Load(IReader* fs)
{
	R_ASSERT(0!=g_pGameLevel);
	R_ASSERT(!b_loaded);

	// Begin
	pApp->LoadBegin();
	kt_base_reading = false;
	kt_base_on = true;
	kt_prepare_reset();
	dxRenderDeviceRender::Instance().Resources->DeferredLoad(TRUE);
	IReader* chunk;

	// Shaders
	//	g_pGamePersistent->LoadTitle		("st_loading_shaders");
	g_pGamePersistent->LoadTitle();
	{
		chunk = fs->open_chunk(fsL_SHADERS);
		R_ASSERT2(chunk, "Level doesn't builded correctly.");
		u32 count = chunk->r_u32();
		Shaders.resize(count);
		for (u32 i = 0; i < count; i++) // skip first shader as "reserved" one
		{
			string512 n_sh, n_tlist;
			LPCSTR n = LPCSTR(chunk->pointer());
			chunk->skip_stringZ();
			if (0 == n[0]) continue;
			xr_strcpy(n_sh, n);
			LPSTR delim = strchr(n_sh, '/');
			*delim = 0;
			xr_strcpy(n_tlist, delim + 1);
			Shaders[i] = dxRenderDeviceRender::Instance().Resources->Create(n_sh, n_tlist);
			if (kt_screen.size() <= i) kt_screen.resize(i + 1, 0);
			kt_screen[i] = (strstr(n_tlist, "prop_fake") || strstr(n_tlist, "fx_lavahell")) ? 1 : (strstr(n_tlist, "terrain\\") == n_tlist ? 2 : 0);
		}
		chunk->close();
	}

	// Components
	Wallmarks = xr_new<CWallmarksEngine>();
	Details = xr_new<CDetailManager>();

	if (!g_dedicated_server)
	{
		// VB,IB,SWI
		//		g_pGamePersistent->LoadTitle("st_loading_geometry");
		g_pGamePersistent->LoadTitle();
		// kt: which of the level's visuals a cut box touches, to drop their triangles while the buffers are read
		if (g_kt_chunk.active)
		{
			chunk = fs->open_chunk(fsL_VISUALS);
			kt_CutScan(chunk);
			chunk->close();
		}
		{
			CStreamReader* geom = FS.rs_open("$level$", "level.geom");
			R_ASSERT2(geom, "level.geom");
			LoadBuffers(geom,FALSE);
			LoadSWIs(geom);
			FS.r_close(geom);
		}

		//...and alternate/fast geometry
		{
			CStreamReader* geom = FS.rs_open("$level$", "level.geomx");
			R_ASSERT2(geom, "level.geomX");
			LoadBuffers(geom,TRUE);
			FS.r_close(geom);
		}

		kt_cut_ranges.clear();

		// Visuals
		//		g_pGamePersistent->LoadTitle("st_loading_spatial_db");
		g_pGamePersistent->LoadTitle();
		chunk = fs->open_chunk(fsL_VISUALS);
		LoadVisuals(chunk);
		chunk->close();

		// Details
		//		g_pGamePersistent->LoadTitle("st_loading_details");
		g_pGamePersistent->LoadTitle();
		Details->Load();
	}

	// Sectors
	//	g_pGamePersistent->LoadTitle("st_loading_sectors_portals");
	g_pGamePersistent->LoadTitle();
	LoadSectors(fs);

	kt_base.shaders = u32(Shaders.size()); kt_base.visuals = u32(Visuals.size()); kt_base.sectors = u32(Sectors.size());
	kt_base.vb = u32(nVB.size()); kt_base.ib = u32(nIB.size()); kt_base.xvb = u32(xVB.size()); kt_base.xib = u32(xIB.size());
	kt_base_on = true;
	kt_base_reading = false;

	// Seamless world experiment (kt): the static geometry of one more level, shifted
	if (g_kt_chunk.active && !g_dedicated_server)
		for (u32 piece = 0; piece < g_kt_chunk.pieces.size(); piece++)
		{
			CTimer kt_timer;
			kt_timer.Start();
			kt_LoadChunk(piece);
			Msg("* [kt time] render of %s: %u ms", g_kt_chunk.pieces[piece].name, kt_timer.GetElapsed_ms());
		}

	// 3D Fluid
	Load3DFluid();

	// HOM
	HOM.Load();

	// Lights
	// pApp->LoadTitle			("Loading lights...");
	LoadLights(fs);

	// End
	pApp->LoadEnd();

	// signal loaded
	b_loaded = TRUE;
}

void CRender::level_Unload()
{
	if (0 == g_pGameLevel) return;
	if (!b_loaded) return;

	GMBase.clear();
	GMRainWet.clear();
	for (sun::cascade& cascade : m_sun_cascades)
		cascade.GMCascade.clear();

	u32 I;

	// HOM
	HOM.Unload();

	//*** Details
	Details->Unload();

	//*** Sectors
	// 1.
	xr_delete(rmPortals);
	pLastSector = 0;
	pOutdoorSector = 0;
	vLastCameraPos.set(0, 0, 0);
	// 2.
	g_kt_chunk.sectors.clear();
	kt_screen.clear();
	for (I = 0; I < Sectors.size(); I++) xr_delete(Sectors[I]);
	Sectors.clear();
	// 3.
	for (I = 0; I < Portals.size(); I++) xr_delete(Portals[I]);
	Portals.clear();

	//*** Lights
	// Glows.Unload			();
	Lights.Unload();

	//*** Visuals
	for (I = 0; I < Visuals.size(); I++)
	{
		if (!Visuals[I]) continue; // a piece let go on the move
		Visuals[I]->Release();
		xr_delete(Visuals[I]);
	}
	Visuals.clear();
	kt_loaded.clear();

	//*** SWI
	for (I = 0; I < SWIs.size(); I++)xr_free(SWIs[I].sw);
	SWIs.clear();
	for (u32 set = 0; set < kt_SWIs.size(); set++)
	{
		for (I = 0; I < kt_SWIs[set]->size(); I++)xr_free((*kt_SWIs[set])[I].sw);
		xr_delete(kt_SWIs[set]);
	}
	kt_SWIs.clear();

	//*** VB/IB
	for (I = 0; I < nVB.size(); I++) _RELEASE(nVB[I]);
	for (I = 0; I < xVB.size(); I++) _RELEASE(xVB[I]);
	nVB.clear();
	xVB.clear();
	for (I = 0; I < nIB.size(); I++) _RELEASE(nIB[I]);
	for (I = 0; I < xIB.size(); I++) _RELEASE(xIB[I]);
	nIB.clear();
	xIB.clear();
	nDC.clear();
	xDC.clear();

	//*** Components
	xr_delete(Details);
	xr_delete(Wallmarks);

	//*** Shaders
	Shaders.clear_and_free();

	const bool clearResources = psDeviceFlags2.test(rsClearAllResources);
	if (psDeviceFlags2.test(rsClearModels) || clearResources)
	{
		Models->ClearPool(true);
		Visuals.clear_and_free();
		if (clearResources)
		{
			dxRenderDeviceRender::Instance().Resources->UnloadAllTexturesOnLevelUnload();
			dxRenderDeviceRender::Instance().ResourcesDestroyNecessaryTextures();
			dxRenderDeviceRender::Instance().Resources->Evict();
		}
		dxRenderDeviceRender::Instance().Resources->Dump(false);
		//static int unload_counter = 0;
		//Msg("The Level Unloaded.======================== %d", ++unload_counter);
	}

	b_loaded = FALSE;
}

// kt: vertex data kept while one geometry file is read, see the cut boxes
// kt: the index ranges (buffer of the piece being read, base) of the visuals left without a triangle
xr_vector<std::pair<u32, u32> > kt_all_cut;
u32 kt_all_cut_hidden = 0;

struct kt_kept_vb
{
	BYTE* data;
	u32 stride, position, count;
};
static xr_vector<kt_kept_vb> kt_kept;
static u32 kt_cut_total = 0;

void CRender::LoadBuffers(CStreamReader* base_fs, BOOL _alternative)
{
	R_ASSERT2(base_fs, "Could not load geometry. File not found.");
	dxRenderDeviceRender::Instance().Resources->Evict();
	//	u32	dwUsage					= D3DUSAGE_WRITEONLY;

	xr_vector<VertexDeclarator>& _DC = _alternative ? xDC : nDC;
	xr_vector<ID3DVertexBuffer*>& _VB = _alternative ? xVB : nVB;
	xr_vector<ID3DIndexBuffer*>& _IB = _alternative ? xIB : nIB;

	// Vertex buffers
	{
		// Use DX9-style declarators
		CStreamReader* fs = base_fs->open_chunk(fsL_VB);
		R_ASSERT2(fs, "Could not load geometry. File 'level.geom?' corrupted.");
		u32 count = fs->r_u32();
		// kt: a chunk's buffers go after the level's own
		const u32 vb_base = g_kt_chunk.loading ? u32(_VB.size()) : 0;
		// (the level's own buffers read again on the move go to their old places, the pieces' follow them)
		if (_DC.size() < vb_base + count) _DC.resize(vb_base + count);
		if (_VB.size() < vb_base + count) _VB.resize(vb_base + count);
		u32 bufferSize = (MAXD3DDECLLENGTH + 1) * sizeof(D3DVERTEXELEMENT9);
		D3DVERTEXELEMENT9* dcl = (D3DVERTEXELEMENT9*)_alloca(bufferSize);
		kt_kept.resize(count);
		for (u32 n = 0; n < count; n++)
		{
			const u32 i = vb_base + n;
			kt_kept[n].data = 0;
			// decl
			//			D3DVERTEXELEMENT9*	dcl		= (D3DVERTEXELEMENT9*) fs().pointer();
			fs->r(dcl, bufferSize);
			fs->advance(-(int)bufferSize);

			u32 dcl_len = D3DXGetDeclLength(dcl) + 1;
			_DC[i].resize(dcl_len);
			fs->r(_DC[i].begin(), dcl_len * sizeof(D3DVERTEXELEMENT9));

			// count, size
			u32 vCount = fs->r_u32();
			u32 vSize = D3DXGetDeclVertexSize(dcl, 0);
			Msg("* [Loading VB] %d verts, %d Kb", vCount, (vCount * vSize) / 1024);

			// Create and fill
			//BYTE*	pData		= 0;
			//R_CHK				(HW.pDevice->CreateVertexBuffer(vCount*vSize,dwUsage,0,D3DPOOL_MANAGED,&_VB[i],0));
			//R_CHK				(_VB[i]->Lock(0,0,(void**)&pData,0));
			//			CopyMemory			(pData,fs().pointer(),vCount*vSize);
			//fs->r				(pData,vCount*vSize);
			//_VB[i]->Unlock		();
			//	TODO: DX10: Check fragmentation.
			//	Check if buffer is less then 2048 kb
			BYTE* pData = xr_alloc<BYTE>(vCount * vSize);
			fs->r(pData, vCount * vSize);
			if (g_kt_chunk.loading)
			{
				// kt: positions stored as three floats are in world space, shift them with the chunk.
				// The vertices of trees are local to the tree and move with its own matrix: left alone.
				xr_vector<u8> local(vCount, 0);
				for (u32 r = 0; r < kt_local_ranges.size(); r++)
				{
					const kt_cut_range& R = kt_local_ranges[r];
					if (R.alt != u32(_alternative ? 1 : 0) || R.vb != n) continue;
					for (u32 v = R.vbase; v < R.vbase + R.vcount && v < vCount; v++) local[v] = 1;
				}
				int moved = 0;
				for (D3DVERTEXELEMENT9* e = dcl; e->Stream != 0xff; e++)
				{
					if (e->Usage != D3DDECLUSAGE_POSITION || e->UsageIndex != 0 || e->Type != D3DDECLTYPE_FLOAT3) continue;
					for (u32 v = 0; v < vCount; v++)
						if (!local[v]) ((Fvector*)(pData + v * vSize + e->Offset))->add(g_kt_chunk.offset);
					moved = 1;
				}
				Msg("* [kt chunk] VB %d: %d verts, positions %s", i, vCount, moved ? "shifted" : "left as they are");
			}
			dx10BufferUtils::CreateVertexBuffer(&_VB[i], pData, vCount * vSize);
			// kt: the vertices of a buffer a cut box may touch are kept till the indices are read
			bool keep = false;
			for (u32 r = 0; r < kt_cut_ranges.size(); r++)
				if (kt_cut_ranges[r].alt == u32(_alternative ? 1 : 0) && kt_cut_ranges[r].vb == n) keep = true;
			if (keep)
				for (D3DVERTEXELEMENT9* e = dcl; e->Stream != 0xff; e++)
					if (e->Usage == D3DDECLUSAGE_POSITION && e->UsageIndex == 0 && e->Type == D3DDECLTYPE_FLOAT3)
					{
						kt_kept[n].data = pData;
						kt_kept[n].stride = vSize;
						kt_kept[n].position = e->Offset;
						kt_kept[n].count = vCount;
					}
			if (!kt_kept[n].data)
			xr_free(pData);

			//			fs->advance			(vCount*vSize);
		}
		fs->close();
	}

	// Index buffers
	{
		CStreamReader* fs = base_fs->open_chunk(fsL_IB);
		u32 count = fs->r_u32();
		const u32 ib_base = g_kt_chunk.loading ? u32(_IB.size()) : 0;
		if (_IB.size() < ib_base + count) _IB.resize(ib_base + count);
		for (u32 n = 0; n < count; n++)
		{
			const u32 i = ib_base + n;
			u32 iCount = fs->r_u32();
			Msg("* [Loading IB] %d indices, %d Kb", iCount, (iCount * 2) / 1024);

			// Create and fill
			//BYTE*	pData		= 0;
			//R_CHK				(HW.pDevice->CreateIndexBuffer(iCount*2,dwUsage,D3DFMT_INDEX16,D3DPOOL_MANAGED,&_IB[i],0));
			//R_CHK				(_IB[i]->Lock(0,0,(void**)&pData,0));
			//			CopyMemory			(pData,fs().pointer(),iCount*2);
			//fs->r				(pData,iCount*2);
			//_IB[i]->Unlock		();

			//	TODO: DX10: Check fragmentation.
			//	Check if buffer is less then 2048 kb
			BYTE* pData = xr_alloc<BYTE>(iCount * 2);
			fs->r(pData, iCount * 2);
			// kt: a triangle wholly inside a cut box is made empty (three equal indices) and is not drawn
			for (u32 r = 0; r < kt_cut_ranges.size(); r++)
			{
				const kt_cut_range& R = kt_cut_ranges[r];
				if (R.alt != u32(_alternative ? 1 : 0) || R.ib != n || R.vb >= kt_kept.size() || !kt_kept[R.vb].data) continue;
				const kt_kept_vb& K = kt_kept[R.vb];
				if (R.ibase + R.icount > iCount || R.vbase + R.vcount > K.count) continue;
				u16* idx = (u16*)pData + R.ibase;
				u32 dropped = 0;
				for (u32 t = 0; t + 2 < R.icount; t += 3)
				{
					if (u32(idx[t]) >= R.vcount || u32(idx[t + 1]) >= R.vcount || u32(idx[t + 2]) >= R.vcount) continue;
					const Fvector& a = *(const Fvector*)(K.data + (R.vbase + idx[t]) * K.stride + K.position);
					const Fvector& b = *(const Fvector*)(K.data + (R.vbase + idx[t + 1]) * K.stride + K.position);
					const Fvector& c = *(const Fvector*)(K.data + (R.vbase + idx[t + 2]) * K.stride + K.position);
					if (!g_kt_chunk.cut(g_kt_chunk.reading(), a, b, c, R.strict == 1, R.strict == 2)) continue;
					idx[t + 1] = idx[t + 2] = idx[t];
					dropped++;
				}
				kt_cut_total += dropped;
				// a visual whose every triangle went is not to be drawn at all: empty, it still cost a draw
				if (!_alternative && R.icount >= 3 && dropped * 3 == R.icount) kt_all_cut.push_back(std::make_pair(R.ib, R.ibase));
			}
			dx10BufferUtils::CreateIndexBuffer(&_IB[i], pData, iCount * 2);
			xr_free(pData);

			//			fs().advance		(iCount*2);
		}
		fs->close();
	}
	for (u32 n = 0; n < kt_kept.size(); n++) xr_free(kt_kept[n].data);
	kt_kept.clear();
	if (kt_cut_total) Msg("* [kt chunk] cut: %u triangles are not drawn (%s buffers); visuals left without a triangle so far: %u", kt_cut_total, _alternative ? "fast" : "main", u32(kt_all_cut.size()));
	kt_cut_total = 0;
}

// kt: remembers where the geometry of the chunk's trees lies.
void CRender::kt_LocalScan(IReader* fs)
{
	kt_local_ranges.clear();
	if (!fs) return;
	IReader* V = 0;
	for (u32 index = 0; (V = fs->open_chunk(index)) != 0; index++)
	{
		ogf_header H;
		if (V->find_chunk(OGF_HEADER))
		{
			V->r(&H, sizeof(H));
			if (H.type == MT_TREE_ST || H.type == MT_TREE_PM)
			{
				kt_cut_range R;
				R.ib = R.ibase = R.icount = R.strict = 0;
				if (V->find_chunk(OGF_GCONTAINER))
				{
					R.alt = 0;
					R.vb = V->r_u32(); R.vbase = V->r_u32(); R.vcount = V->r_u32();
					kt_local_ranges.push_back(R);
				}
				if (V->find_chunk(OGF_FASTPATH))
				{
					IReader* fast = V->open_chunk(OGF_FASTPATH);
					if (fast->find_chunk(OGF_GCONTAINER))
					{
						R.alt = 1;
						R.vb = fast->r_u32(); R.vbase = fast->r_u32(); R.vcount = fast->r_u32();
						kt_local_ranges.push_back(R);
					}
					fast->close();
				}
			}
		}
		V->close();
	}
	Msg("* [kt chunk] trees: %u geometry ranges keep their own coordinates", u32(kt_local_ranges.size()));
}

// kt: finds the visuals whose box touches a cut box and remembers where their geometry lies.
void CRender::kt_CutScan(IReader* fs)
{
	kt_cut_ranges.clear();
	kt_all_cut.clear();
	const bool masked = g_kt_chunk.reading() < g_kt_chunk.mask_pieces && !g_kt_chunk.mask.empty();
	if ((g_kt_chunk.cuts.empty() && !masked) || !fs) return;
	IReader* V = 0;
	for (u32 index = 0; (V = fs->open_chunk(index)) != 0; index++)
	{
		ogf_header H;
		if (V->find_chunk(OGF_HEADER))
		{
			V->r(&H, sizeof(H));
			Fbox box;
			box.set(H.bb.min, H.bb.max);
			if (g_kt_chunk.loading)
			{
				box.min.add(g_kt_chunk.offset);
				box.max.add(g_kt_chunk.offset);
			}
			// under a mask every piece of plain geometry is looked through; trees are hidden whole and
			// never cut: their vertices are the model's own, and one model's geometry is shared by all
			// its trees - a tree whose box touched a cut box had the triangles cut by its model's
			// coordinates, in every tree of the model (Marsh's leaning dead trees lost their trunks
			// all over the level, their broken tops hung in the air; 2026-10-14)
			const bool tree = H.type == MT_TREE_ST || H.type == MT_TREE_PM;
			bool touched = masked && !tree;
			for (u32 c = 0; c < g_kt_chunk.cuts.size() && !tree; c++)
				if (box.intersect(g_kt_chunk.cuts[c])) touched = true;
			if (touched)
			{
				kt_cut_range R;
				const u32 shader = u32(H.shader_id) + (g_kt_chunk.loading ? kt_base_shader : 0);
				R.strict = shader < kt_screen.size() ? kt_screen[shader] : 0; // 1 - a screen, 2 - the terrain
				if (V->find_chunk(OGF_GCONTAINER))
				{
					R.alt = 0;
					R.vb = V->r_u32(); R.vbase = V->r_u32(); R.vcount = V->r_u32();
					R.ib = V->r_u32(); R.ibase = V->r_u32(); R.icount = V->r_u32();
					kt_cut_ranges.push_back(R);
				}
				if (V->find_chunk(OGF_FASTPATH))
				{
					IReader* fast = V->open_chunk(OGF_FASTPATH);
					if (fast->find_chunk(OGF_GCONTAINER))
					{
						R.alt = 1;
						R.vb = fast->r_u32(); R.vbase = fast->r_u32(); R.vcount = fast->r_u32();
						R.ib = fast->r_u32(); R.ibase = fast->r_u32(); R.icount = fast->r_u32();
						kt_cut_ranges.push_back(R);
					}
					fast->close();
				}
			}
		}
		V->close();
	}
	Msg("* [kt chunk] cut: %u geometry ranges of %s to look through", u32(kt_cut_ranges.size()), g_kt_chunk.loading ? "the chunk" : "the level");
}

// Seamless world experiment (kt). Reads the static geometry of one more level into the same
// containers the current level uses and lets it be drawn as extra sectors without portals.
// Left out on purpose: its lights, details, occluders, fog volumes and wallmarks.
void CRender::kt_LoadChunk(u32 piece)
{
	g_kt_chunk.select(piece);
	// the piece's slide windows: a vector of its own, the visuals keep pointers into it
	kt_SWIs.push_back(xr_new<xr_vector<FSlideWindowItem> >());
	string_path dir, file;
	FS.update_path(dir, "$game_levels$", g_kt_chunk.name);

	strconcat(sizeof(file), file, dir, "\\level");
	IReader* fs = FS.r_open(file);
	if (!fs)
	{
		Msg("! [kt chunk] cannot open %s", file);
		return;
	}

	CTimer kt_stage;
	kt_stage.Start();
	u32 kt_ms_shaders = 0, kt_ms_buffers = 0, kt_ms_visuals = 0;
	kt_base_shader = u32(Shaders.size());
	kt_base_visual = u32(Visuals.size());
	kt_base_vb = u32(nVB.size());
	kt_base_ib = u32(nIB.size());
	kt_base_xvb = u32(xVB.size());
	kt_base_xib = u32(xIB.size());
	const u32 sector_base = u32(Sectors.size());
	g_kt_chunk.loading = true;

	// Shaders (see kt_shader_names). Made ahead by kt_piece_prepare, they are only looked up here.
	{
		xr_vector<std::pair<xr_string, xr_string> > names;
		kt_shader_names(piece, names);
		const u32 count = u32(names.size());
		Shaders.resize(kt_base_shader + count);
		if (kt_screen.size() < kt_base_shader + count) kt_screen.resize(kt_base_shader + count, 0);
		for (u32 i = 0; i < count; i++)
		{
			if (names[i].first.empty()) continue;
			LPCSTR n_tlist = names[i].second.c_str();
			Shaders[kt_base_shader + i] = dxRenderDeviceRender::Instance().Resources->Create(names[i].first.c_str(), n_tlist);
			kt_screen[kt_base_shader + i] = (strstr(n_tlist, "prop_fake") || strstr(n_tlist, "fx_lavahell")) ? 1 : (strstr(n_tlist, "terrain\\") == n_tlist ? 2 : 0);
		}
	}

	kt_ms_shaders = kt_stage.GetElapsed_ms();
	// Geometry
	{
		IReader* visuals = fs->open_chunk(fsL_VISUALS);
		kt_CutScan(visuals);
		visuals->close();
		visuals = fs->open_chunk(fsL_VISUALS);
		kt_LocalScan(visuals);
		visuals->close();

		// a patched copy made for the seam (relit, for one) goes before the level's own file
		string_path over;
		strconcat(sizeof(over), over, "kt_override\\", g_kt_chunk.name, "\\level.geom");
		if (!FS.exist(file, "$game_levels$", over))
			strconcat(sizeof(file), file, dir, "\\level.geom");
		else
			Msg("* [kt chunk] geometry of %s is read from %s", g_kt_chunk.name, file);
		CStreamReader* geom = FS.rs_open(0, file);
		R_ASSERT2(geom, file);
		LoadBuffers(geom, FALSE);
		LoadSWIs(geom);
		FS.r_close(geom);

		strconcat(sizeof(file), file, dir, "\\level.geomx");
		if (FS.exist(file))
		{
			geom = FS.rs_open(0, file);
			R_ASSERT2(geom, file);
			LoadBuffers(geom, TRUE);
			FS.r_close(geom);
		}
		kt_cut_ranges.clear();
		kt_local_ranges.clear();
	}

	kt_ms_buffers = kt_stage.GetElapsed_ms();
	// Visuals
	{
		IReader* chunk = fs->open_chunk(fsL_VISUALS);
		LoadVisuals(chunk);
		chunk->close();
	}
	kt_ms_visuals = kt_stage.GetElapsed_ms();

	// Sectors: only their root visuals. The chunk's portals are not read: its sectors are drawn
	// whenever the view sees them (CDSGraphManager::traverse).
	{
		IReader* S = fs->open_chunk(fsL_SECTORS);
		for (u32 i = 0; ; i++)
		{
			IReader* P = S->open_chunk(i);
			if (0 == P) break;
			CSector* sector = xr_new<CSector>();
			u32 size = P->find_chunk(fsP_Root);
			R_ASSERT(size == 4);
			sector->kt_set_root((dxRender_Visual*)getVisual(P->r_u32()));
			Sectors.push_back(sector);
			g_kt_chunk.sectors.push_back(sector);
			P->close();
		}
		S->close();
	}

	g_kt_chunk.loading = false;
	FS.r_close(fs);

	// The chunk's collision triangles name its sectors by its own numbers.
	CDB::TRI* tris = g_pGameLevel->ObjectSpace.GetStaticTris();
	const KtPiece& P = g_kt_chunk.pieces[piece];
	for (u32 i = 0; i < P.tri_count; i++)
		tris[P.tri_base + i].sector += sector_base;

	if (kt_loaded.size() <= piece)
	{
		kt_loaded_piece none;
		ZeroMemory(&none, sizeof(none));
		kt_loaded.resize(piece + 1, none);
	}
	kt_loaded_piece& L = kt_loaded[piece];
	L.on = true;
	L.shader0 = kt_base_shader; L.shader1 = u32(Shaders.size());
	L.visual0 = kt_base_visual; L.visual1 = u32(Visuals.size());
	L.vb0 = kt_base_vb; L.vb1 = u32(nVB.size());
	L.ib0 = kt_base_ib; L.ib1 = u32(nIB.size());
	L.xvb0 = kt_base_xvb; L.xvb1 = u32(xVB.size());
	L.xib0 = kt_base_xib; L.xib1 = u32(xIB.size());
	L.sector0 = sector_base; L.sector1 = u32(Sectors.size());
	L.swis = kt_SWIs.back();

	Msg("* [kt time] render of %s by stages: shaders %u, buffers %u, visuals %u, the rest %u ms", P.name, kt_ms_shaders,
	    kt_ms_buffers - kt_ms_shaders, kt_ms_visuals - kt_ms_buffers, kt_stage.GetElapsed_ms() - kt_ms_visuals);
	Msg("* [kt chunk] render of %s: %u shaders, %u visuals, %u sectors, VB %u, IB %u", P.name, u32(Shaders.size()) - kt_base_shader,
	    u32(Visuals.size()) - kt_base_visual, u32(Sectors.size()) - sector_base, u32(nVB.size()) - kt_base_vb, u32(nIB.size()) - kt_base_ib);
}

// Seamless world experiment (kt): a piece taken out on the move. Its places in the containers stay
// empty; read again, it goes to their ends.
void CRender::kt_UnloadChunk(u32 piece)
{
	if (piece >= kt_loaded.size() || !kt_loaded[piece].on) return;
	kt_loaded_piece& L = kt_loaded[piece];
	u32 I;

	// Sectors. Whatever stood in them (lights, particles, things) is told to look for its sector anew.
	{
		xr_vector<ISpatialShared> all;
		Fvector centre = {0.f, 0.f, 0.f};
		ISpatial_DB* spaces[3] = {g_SpatialSpace, g_SpatialSpaceLights, g_SpatialSpacePhysic};
		for (u32 s = 0; s < 3; s++)
		{
			if (!spaces[s]) continue;
			all.clear();
			spaces[s]->q_sphere(all, 0, u32(-1), centre, 1000000.f);
			for (ISpatialShared spatial : all)
			{
				if (!spatial || !spatial->spatial.sector) continue;
				for (I = L.sector0; I < L.sector1; I++)
					if ((void*)spatial->spatial.sector == (void*)Sectors[I])
					{
						spatial->spatial.sector = nullptr;
						break;
					}
			}
		}
	}
	for (I = L.sector0; I < L.sector1; I++)
	{
		if (pLastSector == Sectors[I]) pLastSector = pOutdoorSector;
		for (u32 k = 0; k < g_kt_chunk.sectors.size(); k++)
			if (g_kt_chunk.sectors[k] == (void*)Sectors[I])
			{
				g_kt_chunk.sectors.erase(g_kt_chunk.sectors.begin() + k);
				break;
			}
		xr_delete(Sectors[I]);
	}

	for (I = L.visual0; I < L.visual1; I++)
	{
		if (!Visuals[I]) continue;
		Visuals[I]->Release();
		xr_delete(Visuals[I]);
	}

	if (L.swis)
	{
		for (I = 0; I < L.swis->size(); I++) xr_free((*L.swis)[I].sw);
		for (u32 k = 0; k < kt_SWIs.size(); k++)
			if (kt_SWIs[k] == L.swis)
			{
				kt_SWIs.erase(kt_SWIs.begin() + k);
				break;
			}
		xr_delete(L.swis);
	}

	for (I = L.vb0; I < L.vb1; I++) _RELEASE(nVB[I]);
	for (I = L.xvb0; I < L.xvb1; I++) _RELEASE(xVB[I]);
	for (I = L.ib0; I < L.ib1; I++) _RELEASE(nIB[I]);
	for (I = L.xib0; I < L.xib1; I++) _RELEASE(xIB[I]);
	for (I = L.shader0; I < L.shader1; I++) Shaders[I] = ref_shader();

	L.on = false;
	Msg("* [kt chunk] render of %s is let go: %u visuals, %u sectors", g_kt_chunk.pieces[piece].name, L.visual1 - L.visual0, L.sector1 - L.sector0);
}

// Seamless world experiment (kt): the level itself let go while the player is deep in a neighbour,
// and read again. Its sectors and portals stay (everything is built on them); the visuals, the
// buffers, the shaders with their lightmaps and the occluders go.
void CRender::kt_base_render(bool on)
{
	u32 I;
	if (!on)
	{
		if (!kt_base_on) return;
		HOM.Unload();
		for (I = 0; I < kt_base.sectors; I++) ((CSector*)Sectors[I])->kt_root(nullptr);
		for (I = 0; I < kt_base.visuals; I++)
		{
			if (!Visuals[I]) continue;
			Visuals[I]->Release();
			xr_delete(Visuals[I]);
		}
		for (I = 0; I < kt_base.vb; I++) _RELEASE(nVB[I]);
		for (I = 0; I < kt_base.xvb; I++) _RELEASE(xVB[I]);
		for (I = 0; I < kt_base.ib; I++) _RELEASE(nIB[I]);
		for (I = 0; I < kt_base.xib; I++) _RELEASE(xIB[I]);
		for (I = 0; I < kt_base.shaders; I++) Shaders[I] = ref_shader();
		kt_base_on = false;
		kt_forget_statics();
		Msg("* [kt chunk] render of the level itself is let go: %u visuals", kt_base.visuals);
		return;
	}
	if (kt_base_on) return;
	IReader* fs = FS.r_open("$level$", "level");
	R_ASSERT2(fs, "kt: the level file");
	IReader* chunk;
	{
		chunk = fs->open_chunk(fsL_SHADERS);
		u32 count = chunk->r_u32();
		for (u32 i = 0; i < count && i < kt_base.shaders; i++)
		{
			string512 n_sh, n_tlist;
			LPCSTR n = LPCSTR(chunk->pointer());
			chunk->skip_stringZ();
			if (0 == n[0]) continue;
			xr_strcpy(n_sh, n);
			LPSTR delim = strchr(n_sh, '/');
			*delim = 0;
			xr_strcpy(n_tlist, delim + 1);
			Shaders[i] = dxRenderDeviceRender::Instance().Resources->Create(n_sh, n_tlist);
		}
		chunk->close();
	}
	kt_base_reading = true;
	chunk = fs->open_chunk(fsL_VISUALS);
	kt_CutScan(chunk);
	chunk->close();
	{
		CStreamReader* geom = FS.rs_open("$level$", "level.geom");
		R_ASSERT2(geom, "level.geom");
		LoadBuffers(geom, FALSE);
		LoadSWIs(geom);
		FS.r_close(geom);
	}
	{
		CStreamReader* geom = FS.rs_open("$level$", "level.geomx");
		R_ASSERT2(geom, "level.geomX");
		LoadBuffers(geom, TRUE);
		FS.r_close(geom);
	}
	kt_cut_ranges.clear();
	chunk = fs->open_chunk(fsL_VISUALS);
	LoadVisuals(chunk);
	chunk->close();
	kt_base_reading = false;
	{
		IReader* S = fs->open_chunk(fsL_SECTORS);
		for (I = 0; I < kt_base.sectors; I++)
		{
			IReader* P = S->open_chunk(I);
			if (0 == P) break;
			if (4 == P->find_chunk(fsP_Root))
				((CSector*)Sectors[I])->kt_root((dxRender_Visual*)getVisual(P->r_u32()));
			P->close();
		}
		S->close();
	}
	FS.r_close(fs);
	HOM.Load();
	kt_base_on = true;
	kt_forget_statics();
	Msg("* [kt chunk] render of the level itself is read again: %u visuals", kt_base.visuals);
}

void CRender::kt_piece_render(u32 piece, bool on)
{
	if (on) kt_LoadChunk(piece);
	else kt_UnloadChunk(piece);
	kt_forget_statics();
}

// The materials of a piece as its level file names them. Lightmaps and LOD atlases are files of the
// level with the same names in every level, so the piece's ones get a prefix the texture loader
// understands: "kt~<piece>~".
void CRender::kt_shader_names(u32 piece, xr_vector<std::pair<xr_string, xr_string> >& names)
{
	names.clear();
	string_path file;
	FS.update_path(file, "$game_levels$", g_kt_chunk.pieces[piece].name);
	xr_strcat(file, "\\level");
	IReader* fs = FS.r_open(file);
	if (!fs) return;
	IReader* chunk = fs->open_chunk(fsL_SHADERS);
	R_ASSERT2(chunk, "kt chunk: no shaders in the level file");
	u32 count = chunk->r_u32();
	names.resize(count);
	for (u32 i = 0; i < count; i++)
	{
		string512 n_sh, n_tlist;
		LPCSTR n = LPCSTR(chunk->pointer());
		chunk->skip_stringZ();
		if (0 == n[0]) continue;
		xr_strcpy(n_sh, n);
		LPSTR delim = strchr(n_sh, '/');
		*delim = 0;
		n_tlist[0] = 0;
		string256 token;
		int tokens = _GetItemCount(delim + 1);
		for (int t = 0; t < tokens; t++)
		{
			_GetItem(delim + 1, t, token);
			if (t) xr_strcat(n_tlist, ",");
			if (0 == strncmp(token, "lmap#", 5) || 0 == strncmp(token, "level_lods", 10))
			{
				string32 prefix;
				xr_sprintf(prefix, "kt~%u~", piece);
				xr_strcat(n_tlist, prefix);
			}
			xr_strcat(n_tlist, token);
		}
		names[i].first = n_sh;
		names[i].second = n_tlist;
	}
	chunk->close();
	FS.r_close(fs);
}

// Called every frame while a piece is on its way in: a few materials are made each time, so that
// reading the piece does not have to load all its lightmaps in one frame.
bool CRender::kt_piece_prepare(u32 piece)
{
	for (u32 i = 0; i < kt_prep_done.size(); i++)
		if (kt_prep_done[i] == piece) return true;
	if (kt_prep_piece != piece)
	{
		kt_prep_piece = piece;
		kt_prep_at = 0;
		kt_shader_names(piece, kt_prep_names);
	}
	CTimer timer;
	timer.Start();
	while (kt_prep_at < kt_prep_names.size() && timer.GetElapsed_ms() < 3)
	{
		const std::pair<xr_string, xr_string>& name = kt_prep_names[kt_prep_at++];
		if (name.first.empty()) continue;
		kt_prep_shaders.push_back(dxRenderDeviceRender::Instance().Resources->Create(name.first.c_str(), name.second.c_str()));
	}
	if (kt_prep_at < kt_prep_names.size()) return false;
	kt_prep_done.push_back(piece);
	kt_prep_piece = u32(-1);
	return true;
}

void CRender::kt_prepare_reset()
{
	kt_prep_shaders.clear();
	kt_prep_done.clear();
	kt_prep_names.clear();
	kt_prep_piece = u32(-1);
	kt_prep_at = 0;
}

// Lights keep the list of static geometry they shadow for as long as they stand still. When
// geometry goes or comes the lists are void: every light is told to gather its own anew.
void CRender::kt_forget_statics()
{
	xr_vector<ISpatialShared> all;
	Fvector centre = {0.f, 0.f, 0.f};
	ISpatial_DB* spaces[2] = {g_SpatialSpace, g_SpatialSpaceLights};
	u32 count = 0;
	for (u32 s = 0; s < 2; s++)
	{
		if (!spaces[s]) continue;
		all.clear();
		spaces[s]->q_sphere(all, 0, STYPE_LIGHTSOURCE, centre, 1000000.f);
		for (ISpatialShared spatial : all)
		{
			if (!spatial) continue;
			light* L = (light*)spatial->dcast_Light();
			if (!L) continue;
			L->GMLight.RGraph.clear_static<false>();
			L->m_moving_frames = 0;
			count++;
		}
	}
	// A light that is switched off is in no spatial base and was missed by the look above; switched
	// on later it drew its shadow by the list it had kept, of geometry that was gone by then (the
	// crash in render_lights after a fast travel, 2026-10-13). Every light there is, then.
	for (light* L : v_all_lights)
	{
		L->GMLight.RGraph.clear_static<false>();
		L->m_moving_frames = 0;
	}
	GMRainWet.RGraph.clear_static<false>();
	for (sun::cascade& cascade : m_sun_cascades)
		cascade.GMCascade.RGraph.clear_static<false>();
}

// After the static collision model was rebuilt its triangles name the piece's sectors by the
// piece's own numbers again.
void CRender::kt_piece_sectors(u32 piece)
{
	if (piece >= kt_loaded.size() || !kt_loaded[piece].on) return;
	CDB::TRI* tris = g_pGameLevel->ObjectSpace.GetStaticTris();
	const KtPiece& P = g_kt_chunk.pieces[piece];
	for (u32 i = 0; i < P.tri_count; i++)
		tris[P.tri_base + i].sector += kt_loaded[piece].sector0;
}

void CRender::LoadVisuals(IReader* fs)
{
	IReader* chunk = 0;
	u32 index = 0;
	dxRender_Visual* V = 0;
	ogf_header H;

	while ((chunk = fs->open_chunk(index)) != 0)
	{
		chunk->r_chunk_safe(OGF_HEADER, &H, sizeof(H));
		V = Models->Instance_Create(H.type);
		V->Load(0, chunk, 0);
		if (kt_base_reading) Visuals[index] = V; // the level's own visuals back at their numbers
		else Visuals.push_back(V);

		chunk->close();
		index++;
	}
}

void CRender::LoadLights(IReader* fs)
{
	// lights
	Lights.Load(fs);
	Lights.LoadHemi();
}

struct b_portal
{
	u16 sector_front;
	u16 sector_back;
	svector<Fvector, 6> vertices;
};

void CRender::LoadSectors(IReader* fs)
{
	// allocate memory for portals
	u32 size = fs->find_chunk(fsL_PORTALS);
	R_ASSERT(0==size%sizeof(b_portal));
	u32 count = size / sizeof(b_portal);
	Portals.resize(count);
	for (u32 c = 0; c < count; c++)
		Portals[c] = xr_new<CPortal>();

	// load sectors
	IReader* S = fs->open_chunk(fsL_SECTORS);
	for (u32 i = 0; ; i++)
	{
		IReader* P = S->open_chunk(i);
		if (0 == P) break;

		CSector* __S = xr_new<CSector>();
		__S->load(*P);
		Sectors.push_back(__S);

		P->close();
	}
	S->close();

	// load portals
	if (count)
	{
		CDB::Collector CL;
		fs->find_chunk(fsL_PORTALS);
		for (u32 i = 0; i < count; i++)
		{
			b_portal P;
			fs->r(&P, sizeof(P));
			CPortal* __P = (CPortal*)Portals[i];
			__P->Setup(P.vertices.begin(), P.vertices.size(),
			           (CSector*)getSector(P.sector_front),
			           (CSector*)getSector(P.sector_back));
			for (u32 j = 2; j < P.vertices.size(); j++)
				CL.add_face_packed_D(
					P.vertices[0], P.vertices[j - 1], P.vertices[j],
					u32(i)
				);
		}
		if (CL.getTS() < 2)
		{
			Fvector v1, v2, v3;
			v1.set(-20000.f, -20000.f, -20000.f);
			v2.set(-20001.f, -20001.f, -20001.f);
			v3.set(-20002.f, -20002.f, -20002.f);
			CL.add_face_packed_D(v1, v2, v3, 0);
		}

		// build portal model
		rmPortals = xr_new<CDB::MODEL>();
		rmPortals->build(CL.getV(), int(CL.getVS()), CL.getT(), int(CL.getTS()));
	}
	else
	{
		rmPortals = 0;
	}

	// debug
	//	for (int d=0; d<Sectors.size(); d++)
	//		Sectors[d]->DebugDump	();

	pLastSector = 0;

	// Search for default sector - assume "default" or "outdoor" sector is the largest one
	//. hack: need to know real outdoor sector
	CSector* largest_sector = 0;
	float largest_sector_vol = 0;
	for (u32 s = 0; s < Sectors.size(); s++)
	{
		CSector* S = (CSector*)Sectors[s];
		dxRender_Visual* V = S->root();
		float vol = V->vis.box.getvolume();
		if (vol > largest_sector_vol)
		{
			largest_sector_vol = vol;
			largest_sector = S;
		}
	}
	pOutdoorSector = largest_sector;
}

void CRender::LoadSWIs(CStreamReader* base_fs)
{
	// allocate memory for portals
	if (base_fs->find_chunk(fsL_SWIS))
	{
		CStreamReader* fs = base_fs->open_chunk(fsL_SWIS);
		u32 item_count = fs->r_u32();

		// kt: a chunk's items live in a vector of their own. Visuals keep pointers to the items, so
		// the level's vector must not grow after its visuals are loaded.
		xr_vector<FSlideWindowItem>& items = g_kt_chunk.loading ? *kt_SWIs.back() : SWIs;
		xr_vector<FSlideWindowItem>::iterator it = items.begin();
		xr_vector<FSlideWindowItem>::iterator it_e = items.end();

		for (; it != it_e; ++it)
			xr_free((*it).sw);

		items.clear_not_free();

		items.resize(item_count);
		for (u32 c = 0; c < item_count; c++)
		{
			FSlideWindowItem& swi = items[c];
			swi.sw = NULL;
			swi.reserved[0] = fs->r_u32();
			swi.reserved[1] = fs->r_u32();
			swi.reserved[2] = fs->r_u32();
			swi.reserved[3] = fs->r_u32();
			swi.count = fs->r_u32();
			VERIFY(NULL==swi.sw);
			swi.sw = xr_alloc<FSlideWindow>(swi.count);
			fs->r(swi.sw, sizeof(FSlideWindow) * swi.count);
		}
		fs->close();
	}
}

void CRender::Load3DFluid()
{
	if (!RImplementation.o.volumetricfog)
		return;

	string_path fn_game;
	if (FS.exist(fn_game, "$level$", "level.fog_vol"))
	{
		IReader* F = FS.r_open(fn_game);
		u16 version = F->r_u16();

		if (version == 3)
		{
			u32 cnt = F->r_u32();
			for (u32 i = 0; i < cnt; ++i)
			{
				dx103DFluidVolume* pVolume = xr_new<dx103DFluidVolume>();
				pVolume->Load("", F, 0);

				//	Attach to sector's static geometry
				CSector* pSector = (CSector*)detectSector(pVolume->getVisData().sphere.P);
				//	3DFluid volume must be in render sector
				VERIFY(pSector);

				dxRender_Visual* pRoot = pSector->root();
				//	Sector must have root
				VERIFY(pRoot);
				VERIFY(pRoot->getType() == MT_HIERRARHY);

				((FHierrarhyVisual*)pRoot)->children.push_back(pVolume);
			}
		}

		FS.r_close(F);
	}
}

// Seamless world experiment (kt): what the renderer thinks of sectors where the camera is. Grass,
// the living and the hands are drawn only when the right sectors count as seen, so when they
// vanish this tells which link of the chain is off.
extern u32 kt_draws_report;
void CRender::kt_report()
{
	kt_draws_report = 2;
	auto number = [&](void* sector) -> int
	{
		if (!sector) return -1;
		for (u32 i = 0; i < Sectors.size(); i++)
			if ((void*)Sectors[i] == sector) return int(i);
		return -2; // not in the list: a pointer to a sector that is no more
	};
	string1024 line = "";
	string64 one;
	for (u32 i = 0; i < Sectors.size(); i++)
	{
		bool extra = false;
		for (void* sector : g_kt_chunk.sectors)
			if (sector == (void*)Sectors[i]) extra = true;
		xr_sprintf(one, " %u%s", i, !Sectors[i] ? ":gone" : extra ? ":piece" : "");
		if (xr_strlen(line) + xr_strlen(one) + 1 < sizeof(line)) xr_strcat(line, one);
	}
	Msg("* [kt sectors]%s", line);
	Msg("* [kt sectors] open-air %d, the camera's (last) %d, the view started in %d, open-air seen %s, pieces' sectors listed %u",
	    number(pOutdoorSector), number(pLastSector), number(GMBase.i_start),
	    pOutdoorSector && GMBase.is_sector_visible(pOutdoorSector) ? "yes" : "NO", u32(g_kt_chunk.sectors.size()));

	const Fvector P = Device.vCameraPosition;
	Fvector dir;
	dir.set(0, -1, 0);
	xrXRC xrc;
	xrc.ray_options(CDB::OPT_ONLYNEAREST);
	if (rmPortals)
	{
		xrc.ray_query(rmPortals, P, dir, 1000.f);
		if (xrc.r_count())
		{
			CDB::TRI* tri = rmPortals->get_tris() + xrc.r_begin()->id;
			CPortal* portal = (CPortal*)Portals[tri->dummy];
			CSector* facing = portal->getSectorFacing(P);
			FHierrarhyVisual* root = facing ? (FHierrarhyVisual*)facing->root() : 0;
			Msg("* [kt sectors] a portal %.1f m below: it faces sector %d, whose box %s the camera", xrc.r_begin()->range, number(facing),
			    root ? (root->vis.box.contains(P) ? "HOLDS" : "does not hold") : "is not there for");
		}
		else Msg("* [kt sectors] no portal below");
	}
	xrc.ray_query(g_pGameLevel->ObjectSpace.GetStaticModel(), P, dir, 1000.f);
	if (xrc.r_count())
	{
		const CDB::RESULT* R = xrc.r_begin();
		int piece = -1;
		for (u32 i = 0; i < g_kt_chunk.pieces.size(); i++)
			if (u32(R->id) >= g_kt_chunk.pieces[i].tri_base && u32(R->id) < g_kt_chunk.pieces[i].tri_base + g_kt_chunk.pieces[i].tri_count) piece = int(i);
		Msg("* [kt sectors] the ground %.1f m below: triangle %d of %s, its sector %d, which is %s", R->range, R->id,
		    piece < 0 ? "the level itself" : g_kt_chunk.pieces[piece].name, int(R->sector),
		    getSector(R->sector) ? "there" : "NOT THERE");
	}
	else Msg("* [kt sectors] no ground below the camera at (%.1f %.1f %.1f)", P.x, P.y, P.z);
}
