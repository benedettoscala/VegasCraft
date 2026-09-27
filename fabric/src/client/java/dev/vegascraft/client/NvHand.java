package dev.vegascraft.client;

import org.joml.Matrix4f;

/**
 * Where Minecraft holds a New Vegas weapon, for New Vegas to hang its real model there. Third
 * person: the pose ItemInHandLayer would draw the held item with, captured while AvatarExporter
 * records the avatar (relative to the player's feet, Minecraft axes, blocks). First person (melee):
 * the pose the first-person hand renderer would draw it with.
 */
public final class NvHand {
	/** True while AvatarExporter submits the player's avatar. */
	public static boolean capturing;
	private static final float[] matrix = new float[12];
	private static boolean valid, seen;

	private NvHand() {
	}

	/** A new avatar capture begins: the previous hand is gone unless the layer sees one again. */
	public static void begin() {
		seen = false;
		capturing = true;
	}

	public static void end() {
		capturing = false;
		valid = seen;
	}

	public static void record(Matrix4f m) {
		copy(m, matrix);
		seen = true;
	}

	/** x axis, y axis, z axis, origin; false when the avatar holds no New Vegas weapon. */
	public static boolean get(float[] out) {
		if (valid) {
			System.arraycopy(matrix, 0, out, 0, 12);
		}
		return valid;
	}

	private static final float[] firstMatrix = new float[12];
	private static boolean firstSeen;
	private static long firstAt;

	/** First person: the pose Minecraft would draw a held New Vegas melee weapon with. */
	public static void recordFirstPerson(Matrix4f m) {
		copy(m, firstMatrix);
		firstSeen = true;
		firstAt = System.nanoTime();
	}

	/** The latest first-person pose (within 0.2 s, else the hand isn't drawn any more). */
	public static boolean getFirstPerson(float[] out) {
		boolean fresh = firstSeen && System.nanoTime() - firstAt < 200_000_000L;
		if (fresh) {
			System.arraycopy(firstMatrix, 0, out, 0, 12);
		}
		return fresh;
	}

	private static void copy(Matrix4f m, float[] matrix) {
		matrix[0] = m.m00(); matrix[1] = m.m01(); matrix[2] = m.m02();
		matrix[3] = m.m10(); matrix[4] = m.m11(); matrix[5] = m.m12();
		matrix[6] = m.m20(); matrix[7] = m.m21(); matrix[8] = m.m22();
		matrix[9] = m.m30(); matrix[10] = m.m31(); matrix[11] = m.m32();
	}

	public static void clear() {
		valid = seen = capturing = false;
	}
}
