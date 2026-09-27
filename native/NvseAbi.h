#pragma once
#include <cstddef>
#include <cstdint>

// Small, cdecl-only ABI surface verified against xNVSE's PluginAPI.h.
// No script commands are registered, so no opcode allocation is needed.
namespace vegas::nvse {
using U32 = std::uint32_t;
using PluginHandle = U32;
struct CommandInfo;
struct ExpressionEvaluatorUtils;
enum CommandReturnType : int;
struct Interface {
    U32 nvseVersion, runtimeVersion, editorVersion, isEditor;
    bool (*RegisterCommand)(CommandInfo*);
    void (*SetOpcodeBase)(U32);
    void* (*QueryInterface)(U32);
    PluginHandle (*GetPluginHandle)();
    bool (*RegisterTypedCommand)(CommandInfo*, CommandReturnType);
    const char* (*GetRuntimeDirectory)();
    U32 isNogore;
    void (*InitExpressionEvaluatorUtils)(ExpressionEvaluatorUtils*);
    bool (*RegisterTypedCommandVersion)(CommandInfo*, CommandReturnType, U32);
};
struct PluginInfo {
    U32 infoVersion;
    const char* name;
    U32 version;
};
struct Message {
    const char* sender;
    U32 type, dataLen;
    void* data;
};
struct Messaging {
    U32 version;
    bool (*RegisterListener)(PluginHandle, const char*, void (*)(Message*));
    bool (*Dispatch)(PluginHandle, U32, void*, U32, const char*);
};
inline constexpr U32 kConsole = 1;
inline constexpr U32 kMessaging = 2;
struct Console {
    U32 version;
    bool (*RunScriptLine)(const char* line, void* callingRef);
};
inline constexpr U32 kRuntime = 0x040020D0; // FalloutNV 1.4.0.525, standard executable.
enum MessageType : U32 {
    PostLoad = 0, ExitGame = 1, ExitToMainMenu = 2,
    LoadGame = 3, SaveGame = 4, ScriptPrecompile = 5,
    PreLoadGame = 6, ExitGameConsole = 7, PostLoadGame = 8,
    PostPostLoad = 9, RuntimeScriptError = 10, DeleteGame = 11,
    RenameGame = 12, RenameNewGame = 13, NewGame = 14,
    DeleteGameName = 15, RenameGameName = 16, RenameNewGameName = 17,
    DeferredInit = 18, ClearScriptDataCache = 19, MainGameLoop = 20,
    ScriptCompile = 21, EventListDestroyed = 22, PostQueryPlugins = 23,
    OnFramePresent = 24, ReloadConfig = 25
};
static_assert(sizeof(void*) == 4);
static_assert(offsetof(Interface, isNogore) == 40);
static_assert(sizeof(PluginInfo) == 12);
static_assert(sizeof(Message) == 16);
} // namespace vegas::nvse
