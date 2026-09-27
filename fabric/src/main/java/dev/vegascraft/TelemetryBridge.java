package dev.vegascraft;

import java.lang.foreign.*;
import java.lang.invoke.MethodHandle;
import java.lang.invoke.VarHandle;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import static java.lang.foreign.ValueLayout.*;

/** Windows x64 Java end of SkyCraft's v11 wire layout; no game or renderer takeover. */
public final class TelemetryBridge implements AutoCloseable {
    public static final String DEFAULT_NAME = "Local\\VegasCraft_v1";
    private static final int MAGIC = 0x43594B53, VERSION = 20;
    private static final long MAPPING_BYTES = 0x20000L + (32L << 20) + 3840L * 2160 * 4 * 3 + (64L << 20);
    private static final long SKY = 0x100, MC = 0x200;
    private static final OfInt I32 = JAVA_INT.withOrder(ByteOrder.LITTLE_ENDIAN);
    private static final OfLong I64 = JAVA_LONG.withOrder(ByteOrder.LITTLE_ENDIAN);
    private static final OfDouble F64 = JAVA_DOUBLE.withOrder(ByteOrder.LITTLE_ENDIAN);
    private static final OfFloat F32 = JAVA_FLOAT.withOrder(ByteOrder.LITTLE_ENDIAN);
    private static final VarHandle INT = I32.varHandle(), LONG = I64.varHandle();
    private static final Arena API_ARENA = Arena.ofShared();
    private static final SymbolLookup KERNEL = SymbolLookup.libraryLookup("kernel32", API_ARENA);
    private static final Linker LINKER = Linker.nativeLinker();
    private static MethodHandle function(String name, FunctionDescriptor signature) {
        return LINKER.downcallHandle(KERNEL.find(name).orElseThrow(), signature);
    }
    private static final MethodHandle OPEN = function("OpenFileMappingW", FunctionDescriptor.of(ADDRESS, JAVA_INT, JAVA_INT, ADDRESS));
    private static final MethodHandle MAP = function("MapViewOfFile", FunctionDescriptor.of(ADDRESS, ADDRESS, JAVA_INT, JAVA_INT, JAVA_INT, JAVA_LONG));
    private static final MethodHandle UNMAP = function("UnmapViewOfFile", FunctionDescriptor.of(JAVA_INT, ADDRESS));
    private static final MethodHandle CLOSE = function("CloseHandle", FunctionDescriptor.of(JAVA_INT, ADDRESS));
    private static final MethodHandle TICK = function("GetTickCount64", FunctionDescriptor.of(JAVA_LONG));
    private static final MethodHandle PID = function("GetCurrentProcessId", FunctionDescriptor.of(JAVA_INT));
    private Arena arena;
    private MemorySegment handle, rawView, memory;
    private final String name;
    private String error = "";

    public record HostState(int flags, int worldId, int epoch, double x, double y, double z,
                            float yaw, float pitch, int teleportSequence) {
        public boolean inGame() { return (flags & 1) != 0; }
    }
    public TelemetryBridge() { this(System.getProperty("vegascraft.link", DEFAULT_NAME)); }
    public TelemetryBridge(String name) { this.name = name; }
    public String error() { return error; }
    public static long tick() {
        try { return (long) TICK.invokeExact(); }
        catch (Throwable e) { throw new IllegalStateException(e); }
    }
    public boolean connect() {
        if (memory != null) return alive();
        try {
            arena = Arena.ofShared();
            MemorySegment text = arena.allocateFrom(name, StandardCharsets.UTF_16LE);
            handle = (MemorySegment) OPEN.invokeExact(0xF001F, 0, text);
            if (handle.address() == 0) { close(); return false; }
            rawView = (MemorySegment) MAP.invokeExact(handle, 0xF001F, 0, 0, MAPPING_BYTES);
            if (rawView.address() == 0) { error = "Mapping has an incompatible size or cannot be mapped"; close(); return false; }
            memory = rawView.reinterpret(MAPPING_BYTES, arena, ignored -> {});
            if ((int) INT.getAcquire(memory, 0L) != MAGIC || memory.get(I32, 4) != VERSION) {
                error = "Protocol mismatch: expected SkyCraft v" + VERSION;
                close(); return false;
            }
            if (!alive()) { close(); return false; }
            memory.set(I32, 12, (int) PID.invokeExact());
            heartbeat();
            error = "";
            return true;
        } catch (Throwable e) {
            error = e.toString(); close(); return false;
        }
    }
    public boolean alive() {
        if (memory == null) return false;
        long beat = (long) LONG.getAcquire(memory, 16L), now = tick();
        return beat > 0 && beat <= now && now - beat < 3000;
    }
    public void heartbeat() {
        if (memory != null) LONG.setRelease(memory, 24L, tick());
    }
    public HostState readHost() {
        if (!alive()) return null;
        for (int attempt = 0; attempt < 8; ++attempt) {
            int seq = (int) INT.getAcquire(memory, SKY);
            if ((seq & 1) != 0) continue;
            HostState state = new HostState(memory.get(I32, SKY + 4), memory.get(I32, SKY + 8),
                memory.get(I32, SKY + 12), memory.get(F64, SKY + 16), memory.get(F64, SKY + 24),
                memory.get(F64, SKY + 32), memory.get(F32, SKY + 40), memory.get(F32, SKY + 44), memory.get(I32, SKY + 48));
            VarHandle.acquireFence();
            if ((int) INT.getAcquire(memory, SKY) == seq && Double.isFinite(state.x()) &&
                Double.isFinite(state.y()) && Double.isFinite(state.z()) && Float.isFinite(state.yaw()) && Float.isFinite(state.pitch())) return state;
        }
        return null;
    }
    public void publish(int flags, double x, double y, double z, float yaw, float pitch, long sample) {
        if (memory == null) return;
        int seq = (int) INT.getAcquire(memory, MC);
        INT.setVolatile(memory, MC, seq + 1);
        memory.set(I32, MC + 4, flags);
        memory.set(F64, MC + 8, x); memory.set(F64, MC + 16, y); memory.set(F64, MC + 24, z);
        memory.set(F32, MC + 32, yaw); memory.set(F32, MC + 36, pitch);
        memory.set(F32, MC + 40, 1.62f);
        memory.set(I64, MC + 56, sample);
        INT.setRelease(memory, MC, seq + 2);
    }
    @Override public void close() {
        try {
            if (memory != null) {
                LONG.setRelease(memory, 24L, 0L);
                INT.setRelease(memory, 12L, 0);
            }
            if (rawView != null && rawView.address() != 0) { int ignored = (int) UNMAP.invokeExact(rawView); }
        } catch (Throwable ignored) {
        } finally {
            memory = null; rawView = null;
            try { if (handle != null && handle.address() != 0) { int ignored = (int) CLOSE.invokeExact(handle); } }
            catch (Throwable ignored) {} finally { handle = null; }
            if (arena != null) { arena.close(); arena = null; }
        }
    }
}
