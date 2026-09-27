package dev.vegascraft.world;

import static dev.vegascraft.link.Proto.*;
import static java.lang.foreign.ValueLayout.JAVA_BYTE;
import static java.lang.foreign.ValueLayout.JAVA_FLOAT;
import static java.lang.foreign.ValueLayout.JAVA_INT;

import java.lang.foreign.MemorySegment;
import java.util.concurrent.ConcurrentHashMap;
import net.minecraft.world.level.ChunkPos;
import org.jspecify.annotations.Nullable;

/**
 * New Vegas' ground under the Minecraft chunks around the player (proto::LandChunk, sent by
 * Collision::updateLand within 6 chunks): its height, texture material, water, and the columns a
 * New Vegas object stands on. Only for the current New Vegas world; dropped with the collision.
 * Any thread.
 */
public final class NvLand {
	private static final ConcurrentHashMap<Long, Chunk> CHUNKS = new ConcurrentHashMap<>();

	private NvLand() {
	}

	/** One chunk's columns, [z * 16 + x]. */
	public record Chunk(int worldId, float[] height, float[] objectTop, byte[] material, byte[] flags, boolean complete) {
		public float height(int x, int z) {
			return this.height[(z & 15) * 16 + (x & 15)];
		}

		public int material(int x, int z) {
			return this.material[(z & 15) * 16 + (x & 15)] & 0xFF;
		}

		public int flags(int x, int z) {
			return this.flags[(z & 15) * 16 + (x & 15)] & 0xFF;
		}

		/** The top of the New Vegas objects standing on column (x, z), or LAND_UNKNOWN for none. */
		public float objectTop(int x, int z) {
			return this.objectTop[(z & 15) * 16 + (x & 15)];
		}

		public boolean known(int x, int z) {
			return this.height(x, z) != LAND_UNKNOWN;
		}

		/** Bare ground (no New Vegas object, water or road on it) at column (x, z). */
		public boolean open(int x, int z) {
			return this.known(x, z) && (this.flags(x, z) & (LAND_BLOCKED | LAND_WATER | LAND_ROAD)) == 0;
		}
	}

	public static @Nullable Chunk get(ChunkPos pos) {
		return CHUNKS.get(pos.pack());
	}

	public static @Nullable Chunk at(int blockX, int blockZ) {
		return CHUNKS.get(ChunkPos.pack(blockX >> 4, blockZ >> 4));
	}

	/** The land's height at block column (x, z), or NaN where it isn't known. */
	public static double height(int x, int z) {
		Chunk c = at(x, z);
		return c == null || !c.known(x, z) ? Double.NaN : c.height(x, z);
	}

	public static int size() {
		return CHUNKS.size();
	}

	static void clear() {
		CHUNKS.clear();
	}

	/** A kColLand message (collision consumer thread). */
	static void read(MemorySegment s, long p) {
		int worldId = s.get(JAVA_INT, p + LC_WORLD_ID);
		int cx = s.get(JAVA_INT, p + LC_CHUNK_X), cz = s.get(JAVA_INT, p + LC_CHUNK_Z);
		float[] height = new float[256], top = new float[256];
		byte[] material = new byte[256], flags = new byte[256];
		boolean complete = true;
		for (int i = 0; i < 256; i++) {
			height[i] = s.get(JAVA_FLOAT, p + LC_HEIGHT + i * 4L);
			top[i] = s.get(JAVA_FLOAT, p + LC_OBJECT_TOP + i * 4L);
			material[i] = s.get(JAVA_BYTE, p + LC_MATERIAL + i);
			flags[i] = s.get(JAVA_BYTE, p + LC_FLAGS + i);
			complete &= height[i] != LAND_UNKNOWN;
		}
		Chunk before = CHUNKS.get(ChunkPos.pack(cx, cz));
		if (before != null && before.complete && !complete && before.worldId == worldId) {
			return;  // the land went out of New Vegas' loaded cells: keep what we had
		}
		CHUNKS.put(ChunkPos.pack(cx, cz), new Chunk(worldId, height, top, material, flags, complete));
	}
}
