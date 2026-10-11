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
// Seamless world experiment (kt): one more level loaded next to the current one, shifted in space.
// Asked for on the command line: -kt_chunk <level folder>,<x>,<y>,<z>
// Only its static geometry and its collision mesh are taken; no AI map, no objects, no details.
struct ENGINE_API KtChunk
{
	bool configured; // asked for on the command line and found
	bool active; // and the level being played is the one it is to stand next to
	string64 base; // -kt_chunk_base <level>: the only level the chunk is added to; empty - any
	bool loading; // the renderer is reading the chunk's files right now
	string64 name;
	Fvector offset;
	u32 tri_base; // first triangle of the chunk in the merged static collision model
	u32 tri_count;
	xr_vector<void*> sectors; // CSector* of the chunk, drawn whenever they are in view
	// -kt_cut <x0>,<y0>,<z0>,<x1>,<y1>,<z1>, any number of them: boxes in the world. A static triangle
	// with all three corners inside one is dropped from collisions and is not drawn: a doorway through
	// a wall that used to close the level.
	xr_vector<Fbox> cuts;
	// -kt_ai_piece <file under $game_levels$>, any number: small AI maps in the world's coordinates on
	// the level's grid, merged like the chunk's map. They fill the passage between the level and the chunk.
	xr_vector<shared_str> ai_pieces;

	bool cut(const Fvector& a, const Fvector& b, const Fvector& c) const
	{
		for (u32 i = 0; i < cuts.size(); i++)
			if (cuts[i].contains(a) && cuts[i].contains(b) && cuts[i].contains(c)) return true;
		return false;
	}

	KtChunk() : configured(false), active(false), loading(false), tri_base(0), tri_count(0)
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
