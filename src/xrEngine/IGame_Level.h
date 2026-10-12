#ifndef igame_level_h_defined
#define igame_level_h_defined

#pragma once

#include "iinputreceiver.h"
#include "xr_object_list.h"
#include "../xrcdb/xr_area.h"

// refs
class ENGINE_API CCameraManager;
class ENGINE_API CCursor;
class ENGINE_API CCustomHUD;
class ENGINE_API ISpatial;

namespace Feel
{
	class ENGINE_API Sound;
}

class ENGINE_API CServerInfo
{
private:
	struct SItem_ServerInfo
	{
		string128 name;
		u32 color;
	};

	enum { max_item = 15 };

	svector<SItem_ServerInfo, max_item> data;

public:
	u32 Size() { return data.size(); }
	void ResetData() { data.clear(); }

	void AddItem(LPCSTR name_, LPCSTR value_, u32 color_ = RGB(255, 255, 255));
	void AddItem(shared_str& name_, LPCSTR value_, u32 color_ = RGB(255, 255, 255));

	IC SItem_ServerInfo& operator[](u32 id)
	{
		VERIFY(id < max_item);
		return data[id];
	}

	CServerInfo()
	{
	};

	~CServerInfo()
	{
	};
};

//-----------------------------------------------------------------------------------------------------------
// Seamless world experiment (kt): more levels loaded next to the current one, shifted in space.
// Asked for on the command line: -kt_chunk <level folder>,<x>,<y>,<z>, any number of them.
// Their static geometry, collision mesh and AI map (if there is one) are taken; no objects, no details.
struct KtPiece
{
	string64 name;
	Fvector offset;
	u32 tri_base; // first triangle of the piece in the merged static collision model
	u32 tri_count;
	// Taking in and letting go on the move (-kt_stream, console kt_piece). `loaded`: its geometry and
	// collisions are there now; `wanted`: they should be. The AI map and the objects stay always.
	bool loaded, wanted, manual;
	bool has_walk; // the piece has an AI map, `walk` is its box in the world
	Fbox walk;
};

struct ENGINE_API KtChunk
{
	xr_vector<KtPiece> pieces;
	bool configured; // asked for on the command line and found
	bool active; // and the level being played is the one it is to stand next to
	string64 base; // -kt_chunk_base <level>: the only level the chunk is added to; empty - any
	bool loading; // the renderer is reading the chunk's files right now
	u32 current; // the piece being read
	string64 name; // of the piece being read
	Fvector offset;
	xr_vector<void*> sectors; // CSector* of the chunk, drawn whenever they are in view
	// -kt_cut <x0>,<y0>,<z0>,<x1>,<y1>,<z1>, any number of them: boxes in the world. A static triangle
	// with all three corners inside one is dropped from collisions and is not drawn: a doorway through
	// a wall that used to close the level.
	xr_vector<Fbox> cuts;
	// -kt_open <x0>,<y0>,<z0>,<x1>,<y1>,<z1>, any number, and -kt_fake <material id>: a collision
	// triangle of that material (materials\fake, the walls only the actor runs into) wholly inside
	// such a box is dropped. A level stops the actor with them short of where its AI map ends; at a
	// seam that is exactly the passage.
	xr_vector<Fbox> opens;
	u32 fake_material;
	bool opened(u32 material, const Fvector& a, const Fvector& b, const Fvector& c) const
	{
		if (material != fake_material) return false;
		for (u32 i = 0; i < opens.size(); i++)
			if (opens[i].contains(a) && opens[i].contains(b) && opens[i].contains(c)) return true;
		return false;
	}
	// -kt_ai_piece <file under $game_levels$>, any number: small AI maps in the world's coordinates on
	// the level's grid, merged like the chunk's map. They fill the passage between the level and the chunk.
	xr_vector<shared_str> ai_pieces;

	void select(u32 piece)
	{
		current = piece;
		xr_strcpy(name, pieces[piece].name);
		offset = pieces[piece].offset;
	}
	bool has(LPCSTR level) const
	{
		for (u32 i = 0; i < pieces.size(); i++)
			if (0 == xr_strcmp(pieces[i].name, level)) return true;
		return false;
	}

	// -kt_mask <file under $game_levels$>: whose land is where. A grid over the world; a cell names the
	// one piece that may have geometry there (0 the level, 1.. the chunks in the order of the keys,
	// 255 anyone). Two levels put side by side overlap with the hills beyond their edges; the mask
	// gives every place to the level that is walked there. Pieces beyond `mask_pieces` (connectors of
	// our own) are not touched.
	float mask_x0, mask_z0, mask_cell;
	u32 mask_nx, mask_nz, mask_pieces;
	xr_vector<u8> mask;

	// A cell of the mask: 255 - anyone's; else the low seven bits name the owner, and the top bit
	// says the ground here is replaced by a strip of our own that blends the two neighbours' relief.
	u8 mask_at(float x, float z) const
	{
		if (mask.empty()) return 255;
		int ix = iFloor((x - mask_x0) / mask_cell), iz = iFloor((z - mask_z0) / mask_cell);
		if (ix < 0 || iz < 0 || ix >= int(mask_nx) || iz >= int(mask_nz)) return 255;
		return mask[u32(iz) * mask_nx + u32(ix)];
	}
	bool foreign(u32 piece, float x, float z) const
	{
		if (piece >= mask_pieces) return false;
		u8 cell = mask_at(x, z);
		return cell != 255 && u32(cell & 0x7f) != piece;
	}
	bool blended(u32 piece, float x, float z) const
	{
		if (piece >= mask_pieces) return false;
		u8 cell = mask_at(x, z);
		return cell != 255 && (cell & 0x80);
	}
	// -kt_ground <id>,<id>,...: the game materials that are ground (earth, gravel...), for collisions
	xr_vector<u32> ground_materials;
	bool ground(u32 material) const
	{
		for (u32 i = 0; i < ground_materials.size(); i++)
			if (ground_materials[i] == material) return true;
		return false;
	}
	// -kt_static_radius <m>: the objects that never leave for offline on the player's level (anomalies,
	// lamps, things, zones) do it beyond this distance, as the living do beyond theirs. With several
	// levels as one place there are too many of them to keep online all at once. 0 - as it was.
	float static_radius;
	// -kt_stream <in>,<out>: a level standing next to this one is let go (geometry, collisions, grass)
	// when the camera is further than <out> metres from its walkable box and taken in again nearer
	// than <in>. 0 - everything stays loaded.
	float stream_in, stream_out;
	// -kt_reach <in>,<out>: within the distance of -kt_stream a level is held only when the player
	// can soon walk there or sees it. Metres of walking over the AI map from the player's node to
	// the nearest node of the level: nearer than <in> it is taken, beyond <out> it may go. A level
	// behind a hill with the only way in through a far tunnel is not wanted on this side. 0 - off.
	float reach_in, reach_out;
	// -kt_sight <file under $game_levels$>,<in>,<out>: from where each level is seen, counted ahead
	// by the tools. A grid over the world; a cell holds, for the level and every level attached
	// (in the order of the keys), the metres to the nearest thing of it that is seen from there:
	// 0xffff - nothing is seen, 0xfffe - nobody looked from here (then the distance alone decides).
	float sight_in, sight_out, sight_x0, sight_z0, sight_cell;
	u32 sight_nx, sight_nz, sight_pieces;
	xr_vector<u16> sight;
	enum { sight_none = 0xffff, sight_unknown = 0xfffe };
	u16 sight_at(u32 piece, float x, float z) const // piece: 0 the level, 1.. the attached ones
	{
		if (sight.empty() || piece >= sight_pieces) return sight_unknown;
		int ix = iFloor((x - sight_x0) / sight_cell), iz = iFloor((z - sight_z0) / sight_cell);
		if (ix < 0 || iz < 0 || ix >= int(sight_nx) || iz >= int(sight_nz)) return sight_unknown;
		return sight[(u32(iz) * sight_nx + u32(ix)) * sight_pieces + piece];
	}
	// Is the land of a piece there for things to stand on: its geometry is read and is not about to
	// go. piece: 0 the level, 1.. the attached ones, as the AI map names them.
	bool present(u32 piece) const
	{
		if (piece == 0) return base_loaded && base_wanted;
		if (piece - 1 >= pieces.size()) return true;
		return pieces[piece - 1].loaded && pieces[piece - 1].wanted;
	}
	// -kt_trim <piece>,<px>,<pz>,<nx>,<nz>,<radius>, any number: the AI nodes of the piece (0 the level,
	// 1.. the attached levels) that lie beyond the line through (px, pz) on the side the normal
	// points to, within the radius, are taken out of the map: nothing leads to them. Two levels
	// joined road to road overlap with their tails; each is trimmed at the line of the joint.
	struct Trim
	{
		u32 piece;
		float px, pz, nx, nz, radius;
	};
	xr_vector<Trim> trims;
	bool trimmed(u32 piece, float x, float z) const
	{
		for (u32 i = 0; i < trims.size(); i++)
		{
			const Trim& T = trims[i];
			if (T.piece != piece) continue;
			const float dx = x - T.px, dz = z - T.pz;
			if (dx * T.nx + dz * T.nz > 0.f && dx * dx + dz * dz <= T.radius * T.radius) return true;
		}
		return false;
	}
	// The level itself is let go and taken in by the same rule, when the player is deep in a neighbour.
	bool base_loaded, base_wanted, base_manual, base_has_walk;
	Fbox base_walk;
	// the piece whose files are being read by the renderer: 0 the level, 1.. the chunks
	u32 reading() const { return loading ? current + 1 : 0; }

	// Is the triangle of the piece left out: wholly inside a cut box, or on another piece's land.
	// On whose land a triangle lies is told by its middle. `strict` is for what must not stick out
	// over the neighbour at all - the black planes that screen a level from outside, big far
	// triangles: one corner on foreign land is enough.
	bool cut(u32 piece, const Fvector& a, const Fvector& b, const Fvector& c, bool strict = false, bool is_ground = false) const
	{
		for (u32 i = 0; i < cuts.size(); i++)
			if (cuts[i].contains(a) && cuts[i].contains(b) && cuts[i].contains(c)) return true;
		if (mask.empty() || piece >= mask_pieces) return false;
		const float mx = (a.x + b.x + c.x) / 3, mz = (a.z + b.z + c.z) / 3;
		if (foreign(piece, mx, mz)) return true;
		if (is_ground && blended(piece, mx, mz)) return true;
		return strict && (foreign(piece, a.x, a.z) || foreign(piece, b.x, b.z) || foreign(piece, c.x, c.z));
	}

	KtChunk() : configured(false), active(false), loading(false), current(0), fake_material(u32(-1)), static_radius(0), stream_in(0), stream_out(0), reach_in(0), reach_out(0), sight_in(0), sight_out(0), sight_x0(0), sight_z0(0), sight_cell(1), sight_nx(0), sight_nz(0), sight_pieces(0), base_loaded(true), base_wanted(true), base_manual(false), base_has_walk(false), mask_x0(0), mask_z0(0), mask_cell(1), mask_nx(0), mask_nz(0), mask_pieces(0)
	{
		name[0] = 0;
		base[0] = 0;
		offset.set(0, 0, 0);
	}
};

extern ENGINE_API KtChunk g_kt_chunk;
// Is the chunk to be added to the level that is being loaded now. Reads the keys on the first call.
// The AI map is read on its own thread next to the level, so it asks by itself and does not look at
// g_kt_chunk.active, which the level sets for the rest of the game.
ENGINE_API bool kt_chunk_wanted();
// Were the keys given at all, whatever level is being loaded (asked before the level is known).
ENGINE_API bool kt_chunk_keys();

class ENGINE_API IGame_Level :
	public DLL_Pure,
	public IInputReceiver,
	public pureRender,
	public pureFrame,
	public IEventReceiver
{
protected:
	// Network interface
	CObject* pCurrentEntity;
	CObject* pCurrentViewEntity;

	// Static sounds
	xr_vector<ref_sound> Sounds_Random;
	u32 Sounds_Random_dwNextTime;
	BOOL Sounds_Random_Enabled;
	CCameraManager* m_pCameras;

	// temporary
	xr_vector<ISpatialShared> snd_ER;
public:
	CObjectList Objects;
	CObjectSpace ObjectSpace;
	CCameraManager& Cameras() { return *m_pCameras; };

	BOOL bReady;

	CInifile* pLevel;
public: // deferred sound events
	struct _esound_delegate
	{
		Feel::Sound* dest;
		ref_sound_data_ptr source;
		float power;
	};

	xr_vector<_esound_delegate> snd_Events;
public:
	// Main, global functions
	IGame_Level();
	virtual ~IGame_Level();

	virtual shared_str name() const = 0;
	virtual void GetLevelInfo(CServerInfo* si) = 0;

	virtual bool net_Start(const char* op_server, const char* op_client) = 0;
	virtual void net_Load(const char* name) = 0;
	virtual void net_Save(const char* name) = 0;
	virtual void net_Stop();
	virtual void net_Update() = 0;

	virtual bool Load(u32 dwNum);
	virtual bool Load_GameSpecific_Before() { return TRUE; }; // before object loading
	virtual bool Load_GameSpecific_After() { return TRUE; }; // after object loading
	virtual void Load_GameSpecific_CFORM(CDB::TRI* T, u32 count) = 0;

	virtual void _BCL OnFrame(void);
	void kt_stream_tick(); // seamless world experiment (kt): pieces taken in and let go on the move
	// kt: metres of walking from the player to the level (0) and to every attached piece (1..);
	// flt_max - further than -kt_reach looks. False: not known now, the distance alone decides.
	virtual bool kt_reach(xr_vector<float>& metres) { return false; }
	virtual void OnRender(void);

	virtual shared_str OpenDemoFile(const char* demo_file_name) = 0;
	virtual void net_StartPlayDemo() = 0;

	// Main interface
	CObject* CurrentEntity(void) const { return pCurrentEntity; }
	CObject* CurrentViewEntity(void) const { return pCurrentViewEntity; }
	void SetEntity(CObject* O); // { pCurrentEntity=pCurrentViewEntity=O; }
	void SetViewEntity(CObject* O); // { pCurrentViewEntity=O; }

	void SoundEvent_Register(ref_sound_data_ptr S, float range);
	void SoundEvent_Dispatch();
	void SoundEvent_OnDestDestroy(Feel::Sound*);

	// Loader interface
	//ref_shader LL_CreateShader (int S, int T, int M, int C);
	void LL_CheckTextures();
	virtual void SetEnvironmentGameTimeFactor(u64 const& GameTime, float const& fTimeFactor) = 0;
};

//-----------------------------------------------------------------------------------------------------------
extern ENGINE_API IGame_Level* g_pGameLevel;

template <typename _class_type>
void relcase_register(_class_type* self, void (xr_stdcall _class_type::* function_to_bind)(CObject*))
{
	g_pGameLevel->Objects.relcase_register(
		CObjectList::RELCASE_CALLBACK(
			self,
			function_to_bind)
	);
}

template <typename _class_type>
void relcase_unregister(_class_type* self, void (xr_stdcall _class_type::* function_to_bind)(CObject*))
{
	g_pGameLevel->Objects.relcase_unregister(
		CObjectList::RELCASE_CALLBACK(
			self,
			function_to_bind)
	);
}
#endif
