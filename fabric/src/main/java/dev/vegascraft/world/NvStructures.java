package dev.vegascraft.world;

import static dev.vegascraft.link.Proto.*;

import dev.vegascraft.SkyCraft;
import java.util.ArrayList;
import java.util.List;
import net.minecraft.core.BlockPos;
import net.minecraft.core.registries.Registries;
import net.minecraft.resources.Identifier;
import net.minecraft.resources.ResourceKey;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.util.RandomSource;
import net.minecraft.world.level.ChunkPos;
import net.minecraft.world.level.block.Rotation;
import net.minecraft.world.level.levelgen.structure.BoundingBox;
import net.minecraft.world.level.levelgen.structure.templatesystem.BlockIgnoreProcessor;
import net.minecraft.world.level.levelgen.structure.templatesystem.JigsawReplacementProcessor;
import net.minecraft.world.level.levelgen.structure.templatesystem.StructurePlaceSettings;
import net.minecraft.world.level.levelgen.structure.templatesystem.StructureTemplate;

/**
 * Small Minecraft structures on the Mojave: one-piece vanilla templates (camps, ruined portals,
 * fossils, igloos) and the desert well. The world is cut into cells of CELL x CELL chunks; each
 * cell has one candidate chunk, which gets a structure fitting its biome if the ground under the
 * footprint is open and fairly flat. A foundation of the ground's material fills the dips under it.
 */
final class NvStructures {
	private static final int CELL = 6;          // chunks
	private static final float CHANCE = 0.7F;
	private static final int MAX_STEP = 4;       // blocks between the footprint's lowest and highest ground

	// Why footprints were turned down: unknown, water, road, uneven, an object above the floor, crowded.
	private static final int[] REJECTS = new int[6];

	private NvStructures() {
	}

	private static List<String> templatesFor(NvDecorator.Kind kind) {
		List<String> out = new ArrayList<>();
		switch (kind) {
			case DESERT -> {
				out.add("well");  // the desert well feature
				for (String f : new String[] { "skull_1", "skull_2", "skull_3", "spine_1", "spine_2", "spine_3" }) {
					out.add("fossil/" + f);
				}
				out.add("ruined_portal/portal_" + 1);
				out.add("ruined_portal/portal_" + 4);
			}
			case SAVANNA -> {
				for (int i = 1; i <= 4; i++) {
					out.add("abandoned_camp/camp/savanna/campsite_savanna_" + i);
				}
				out.add("ruined_portal/portal_" + 2);
				out.add("fossil/skull_4");
			}
			case PLAINS -> {
				for (int i = 1; i <= 15; i += 2) {
					out.add("abandoned_camp/camp/default/campsite_default_chest_" + i);
				}
			}
			case HILLS -> {
				for (int i = 1; i <= 10; i++) {
					out.add("ruined_portal/portal_" + i);
				}
			}
			case SNOWY -> out.add("igloo/top");
		}
		return out;
	}

	/** The candidate chunk of a cell, and whether it gets anything at all. */
	static boolean isCandidate(long seed, ChunkPos pos) {
		int cellX = Math.floorDiv(pos.x(), CELL), cellZ = Math.floorDiv(pos.z(), CELL);
		long h = NvDecorator.mix(seed ^ 0x5EEDL ^ ((long) cellX * 73428767L) ^ ((long) cellZ * 912931L));
		int ox = (int) Math.floorMod(h, CELL), oz = (int) Math.floorMod(h >>> 20, CELL);
		return pos.x() == cellX * CELL + ox && pos.z() == cellZ * CELL + oz && ((h >>> 40) & 0xFFFF) < CHANCE * 0x10000;
	}

	/** Places this chunk's structure, if it's a candidate. Returns 1 if one was placed. */
	static int maybePlace(ServerLevel level, ChunkPos pos, NvDecorator.Kind kind, RandomSource random) {
		if (!isCandidate(level.getSeed(), pos)) {
			return 0;
		}
		List<String> names = templatesFor(kind);
		String name = names.get(random.nextInt(names.size()));
		// A few spots in the chunk: towns and rocks cover much of the Mojave.
		for (int attempt = 0; attempt < 8; attempt++) {
			int x = pos.getMinBlockX() + 2 + random.nextInt(12), z = pos.getMinBlockZ() + 2 + random.nextInt(12);
			boolean ok = name.equals("well") ? placeWell(level, x, z, random) : placeTemplate(level, Identifier.withDefaultNamespace(name), x, z, random);
			if (ok) {
				SkyCraft.LOG.info("SkyCraft: structure {} at {} {} ({})", name, x, z, kind);
				return 1;
			}
		}
		SkyCraft.LOG.info("SkyCraft: no open ground for structure {} in chunk {} {} ({}); so far turned down for unknown/water/road/uneven/object/crowded {}", name, pos.x(), pos.z(), kind, java.util.Arrays.toString(REJECTS));
		return 0;
	}

	private static boolean placeWell(ServerLevel level, int x, int z, RandomSource random) {
		var feature = level.registryAccess().lookupOrThrow(Registries.FEATURE)
			.get(ResourceKey.create(Registries.FEATURE, Identifier.withDefaultNamespace("desert_well")));
		BoundingBox box = new BoundingBox(x - 2, 0, z - 2, x + 2, 0, z + 2);
		Integer base = foundation(level, box, 0);  // the feature sinks it itself
		return feature.isPresent() && base != null && feature.get().value().place(level, level.getChunkSource().getGenerator(), random, new BlockPos(x, base, z));
	}

	private static boolean placeTemplate(ServerLevel level, Identifier id, int x, int z, RandomSource random) {
		var template = level.getStructureTemplateManager().get(id);
		if (template.isEmpty()) {
			SkyCraft.LOG.warn("SkyCraft: no structure template {}", id);
			return false;
		}
		StructureTemplate t = template.get();
		StructurePlaceSettings settings = new StructurePlaceSettings()
			.setRotation(Rotation.getRandom(random))
			.setRandom(random)
			.addProcessor(BlockIgnoreProcessor.STRUCTURE_AND_AIR)
			.addProcessor(JigsawReplacementProcessor.INSTANCE);
		var size = t.getSize(settings.getRotation());
		BlockPos corner = new BlockPos(x - size.getX() / 2, 0, z - size.getZ() / 2);
		BoundingBox footprint = t.getBoundingBox(settings, corner);
		Integer base = foundation(level, footprint, id.getPath().startsWith("fossil/") ? 2 : 0);  // fossils lie half buried
		if (base == null) {
			return false;
		}
		BlockPos origin = corner.atY(base);
		return t.placeInWorld(level, origin, origin, settings, random, 2);
	}

	/**
	 * The y a structure's floor goes on over this footprint (its ground's mean height, or `sink`
	 * blocks below it), with the ground's material filled in up to it. Null if the ground
	 * there is under water, a road, too uneven, or has New Vegas objects standing above the floor
	 * (small ones, rocks and shrubs, end up inside the foundation).
	 */
	private static Integer foundation(ServerLevel level, BoundingBox box, int sink) {
		double lo = Double.MAX_VALUE, hi = -Double.MAX_VALUE, sum = 0;
		for (int x = box.minX(); x <= box.maxX(); x++) {
			for (int z = box.minZ(); z <= box.maxZ(); z++) {
				NvLand.Chunk c = NvLand.at(x, z);
				if (c == null || !c.known(x, z)) {
					REJECTS[0]++;
					return null;
				}
				if ((c.flags(x, z) & (LAND_WATER | LAND_ROAD)) != 0) {
					REJECTS[(c.flags(x, z) & LAND_WATER) != 0 ? 1 : 2]++;
					return null;
				}
				lo = Math.min(lo, c.height(x, z));
				hi = Math.max(hi, c.height(x, z));
				sum += c.height(x, z);
			}
		}
		if (hi - lo > MAX_STEP) {
			REJECTS[3]++;
			return null;
		}
		int columns = (box.maxX() - box.minX() + 1) * (box.maxZ() - box.minZ() + 1);
		// At the ground's mean height: uphill it sinks into New Vegas' ground, downhill the
		// foundation holds it up.
		int base = (int) Math.round(sum / columns) - sink;
		int covered = 0;
		for (int x = box.minX(); x <= box.maxX(); x++) {
			for (int z = box.minZ(); z <= box.maxZ(); z++) {
				NvLand.Chunk c = NvLand.at(x, z);
				if ((c.flags(x, z) & LAND_BLOCKED) == 0) {
					continue;
				}
				if (c.objectTop(x, z) > base + 0.25F) {
					REJECTS[4]++;
					return null;
				}
				covered++;
			}
		}
		if (covered * 4 > columns) {
			REJECTS[5]++;
			return null;
		}
		for (int x = box.minX(); x <= box.maxX(); x++) {
			for (int z = box.minZ(); z <= box.maxZ(); z++) {
				var ground = NvDecorator.groundState(x, z);
				for (int y = (int) Math.floor(NvLand.height(x, z) - 0.5); y < base; y++) {
					BlockPos p = new BlockPos(x, y, z);
					if (level.getBlockState(p).isAir()) {
						level.setBlock(p, ground, 2);
					}
				}
			}
		}
		return base;
	}
}
