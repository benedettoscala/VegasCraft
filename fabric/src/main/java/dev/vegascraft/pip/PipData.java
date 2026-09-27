package dev.vegascraft.pip;

import static dev.vegascraft.link.Proto.*;
import static java.lang.foreign.ValueLayout.JAVA_BYTE;
import static java.lang.foreign.ValueLayout.JAVA_FLOAT_UNALIGNED;
import static java.lang.foreign.ValueLayout.JAVA_INT;
import static java.lang.foreign.ValueLayout.JAVA_INT_UNALIGNED;
import static java.lang.foreign.ValueLayout.JAVA_SHORT_UNALIGNED;

import dev.vegascraft.SkyCraft;
import java.lang.foreign.MemorySegment;
import java.util.ArrayList;
import java.util.List;

/**
 * What the Pip-Boy's pages show: New Vegas sends it in snapshots, one kind at a time
 * (kColPipData, see PipDataKind in the protocol), and the pages draw the latest of each. Read on the
 * collision consumer's thread; the records are immutable.
 */
public final class PipData {
	private static volatile Stats stats;
	private static volatile Quests quests;
	private static volatile Notes notes;
	private static volatile Radio radio;
	private static volatile WorldMap map;
	private static volatile int version;

	private PipData() {
	}

	// ---- STATS ----
	public record Stat(float cur, float base, String name, String desc, String icon) {
	}

	public record Perk(int form, String name, String desc, int rank, int ranks, String icon) {
	}

	public record Stats(int level, int xp, int xpThis, int xpNext, float hp, float hpMax, float ap, float apMax, float karma, float rads, float dt, float dr,
		float weight, float carry, boolean hardcore, float hunger, float thirst, float sleep, float[] limbCur, float[] limbBase, List<Stat> special,
		List<Stat> skills, List<Perk> perks, int[] misc) {
	}

	// ---- DATA ----
	public record Objective(int id, String text, boolean done) {
	}

	public record Quest(int form, String name, boolean tracked, List<Objective> objectives) {
	}

	public record Quests(List<Quest> quests) {
	}

	public record Note(int form, String name, int type, boolean read, String text) {
	}

	public record Notes(List<Note> notes) {
	}

	public record Station(int ref, String name, boolean inRange, boolean playing) {
	}

	public record Radio(boolean enabled, List<Station> stations) {
	}

	public record Marker(int ref, String name, int type, float x, float y, boolean canTravel) {
	}

	public record WorldMap(int worldId, String texture, float west, float north, float east, float south, float playerX, float playerY, float heading,
		boolean canTravel, List<Marker> markers) {
	}

	public static Stats stats() {
		return stats;
	}

	public static Quests quests() {
		return quests;
	}

	public static Notes notes() {
		return notes;
	}

	public static Radio radio() {
		return radio;
	}

	public static WorldMap map() {
		return map;
	}

	/** Grows with every snapshot: pages redraw from the latest data anyway; this tells them to rebuild lists. */
	public static int version() {
		return version;
	}

	/** One kColPipData payload: PipDataHeader {kind, seq, worldId, bytes} and the data. */
	public static void read(MemorySegment s, long p, int payloadBytes) {
		if (payloadBytes < 16) {
			return;
		}
		int kind = s.get(JAVA_INT, p);
		int bytes = Math.min(s.get(JAVA_INT, p + 12), payloadBytes - 16);
		Reader r = new Reader(s, p + 16, p + 16 + bytes);
		try {
			switch (kind) {
				case PD_STATS -> stats = readStats(r);
				case PD_QUESTS -> quests = readQuests(r);
				case PD_NOTES -> notes = readNotes(r);
				case PD_RADIO -> radio = readRadio(r);
				case PD_MAP -> map = readMap(r, s.get(JAVA_INT, p + 8));
				default -> SkyCraft.LOG.warn("VegasCraft: unknown Pip-Boy data kind {}", kind);
			}
			version++;
		} catch (RuntimeException e) {
			SkyCraft.LOG.warn("VegasCraft: bad Pip-Boy data of kind {}: {}", kind, e.toString());
		}
	}

	private static Stat readStat(Reader r) {
		return new Stat(r.f32(), r.f32(), r.str(), r.str(), r.str());
	}

	private static Stats readStats(Reader r) {
		int level = r.u16(), xp = r.i32(), xpThis = r.i32(), xpNext = r.i32();
		float hp = r.f32(), hpMax = r.f32(), ap = r.f32(), apMax = r.f32(), karma = r.f32(), rads = r.f32(), dt = r.f32(), dr = r.f32();
		float weight = r.f32(), carry = r.f32();
		boolean hardcore = r.u8() != 0;
		float hunger = r.f32(), thirst = r.f32(), sleep = r.f32();
		float[] limbCur = new float[7], limbBase = new float[7];
		for (int i = 0; i < 7; i++) {
			limbCur[i] = r.f32();
			limbBase[i] = r.f32();
		}
		List<Stat> special = new ArrayList<>(), skills = new ArrayList<>();
		for (int i = 0; i < 7; i++) {
			special.add(readStat(r));
		}
		for (int i = 0; i < 14; i++) {
			Stat st = readStat(r);
			if (!st.name().contains("OBSOLETE")) {
				skills.add(st);
			}
		}
		List<Perk> perks = new ArrayList<>();
		for (int n = r.u16(); n > 0; n--) {
			perks.add(new Perk(r.i32(), r.str(), r.str(), r.u8(), r.u8(), r.str()));
		}
		int[] misc = new int[r.u8()];
		for (int i = 0; i < misc.length; i++) {
			misc[i] = r.i32();
		}
		return new Stats(level, xp, xpThis, xpNext, hp, hpMax, ap, apMax, karma, rads, dt, dr, weight, carry, hardcore, hunger, thirst, sleep, limbCur, limbBase,
			special, skills, perks, misc);
	}

	private static Quests readQuests(Reader r) {
		List<Quest> list = new ArrayList<>();
		for (int n = r.u16(); n > 0; n--) {
			int form = r.i32();
			String name = r.str();
			boolean tracked = r.u8() != 0;
			List<Objective> objectives = new ArrayList<>();
			for (int m = r.u16(); m > 0; m--) {
				objectives.add(new Objective(r.i32(), r.str(), r.u8() != 0));
			}
			list.add(new Quest(form, name, tracked, objectives));
		}
		return new Quests(list);
	}

	private static Notes readNotes(Reader r) {
		List<Note> list = new ArrayList<>();
		for (int n = r.u16(); n > 0; n--) {
			list.add(new Note(r.i32(), r.str(), r.u8(), r.u8() != 0, r.str()));
		}
		return new Notes(list);
	}

	private static Radio readRadio(Reader r) {
		boolean enabled = r.u8() != 0;
		List<Station> list = new ArrayList<>();
		for (int n = r.u16(); n > 0; n--) {
			list.add(new Station(r.i32(), r.str(), r.u8() != 0, r.u8() != 0));
		}
		return new Radio(enabled, list);
	}

	private static WorldMap readMap(Reader r, int worldId) {
		String texture = r.str();
		if (texture.isEmpty()) {
			return new WorldMap(worldId, "", 0, 0, 1, 1, 0, 0, 0, false, new ArrayList<>());
		}
		float west = r.f32(), north = r.f32(), east = r.f32(), south = r.f32(), px = r.f32(), py = r.f32(), heading = r.f32();
		boolean canTravel = r.u8() != 0;
		List<Marker> list = new ArrayList<>();
		for (int n = r.u16(); n > 0; n--) {
			list.add(new Marker(r.i32(), r.str(), r.u8(), r.f32(), r.f32(), r.u8() != 0));
		}
		return new WorldMap(worldId, texture, west, north, east, south, px, py, heading, canTravel, list);
	}

	/** Little-endian fields out of the mapped snapshot. */
	private static final class Reader {
		private final MemorySegment s;
		private long at;
		private final long end;

		Reader(MemorySegment s, long at, long end) {
			this.s = s;
			this.at = at;
			this.end = end;
		}

		private void need(int n) {
			if (this.at + n > this.end) {
				throw new IllegalStateException("snapshot ends early");
			}
		}

		int u8() {
			need(1);
			return s.get(JAVA_BYTE, this.at++) & 0xFF;
		}

		int u16() {
			need(2);
			int v = s.get(JAVA_SHORT_UNALIGNED, this.at) & 0xFFFF;
			this.at += 2;
			return v;
		}

		int i32() {
			need(4);
			int v = s.get(JAVA_INT_UNALIGNED, this.at);
			this.at += 4;
			return v;
		}

		float f32() {
			need(4);
			float v = s.get(JAVA_FLOAT_UNALIGNED, this.at);
			this.at += 4;
			return v;
		}

		String str() {
			int n = u16();
			need(n);
			byte[] b = new byte[n];
			for (int i = 0; i < n; i++) {
				b[i] = s.get(JAVA_BYTE, this.at + i);
			}
			this.at += n;
			return new String(b, java.nio.charset.Charset.forName("windows-1252"));
		}
	}
}
