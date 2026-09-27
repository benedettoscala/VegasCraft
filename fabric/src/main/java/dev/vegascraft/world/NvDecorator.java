package dev.vegascraft.world;

import static dev.vegascraft.link.Proto.*;

import com.mojang.serialization.Codec;
import dev.vegascraft.SkyCraft;
import java.util.List;
import net.fabricmc.fabric.api.attachment.v1.AttachmentRegistry;
import net.fabricmc.fabric.api.attachment.v1.AttachmentType;
import net.fabricmc.fabric.api.event.lifecycle.v1.ServerTickEvents;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Holder;
import net.minecraft.core.registries.Registries;
import net.minecraft.resources.Identifier;
import net.minecraft.resources.ResourceKey;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.commands.FillBiomeCommand;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.util.RandomSource;
import net.minecraft.world.level.ChunkPos;
import net.minecraft.world.level.biome.Biome;
import net.minecraft.world.level.biome.Biomes;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.chunk.LevelChunk;
import net.minecraft.world.level.levelgen.feature.Feature;
import org.jspecify.annotations.Nullable;

/**
 * Minecraft's overworld on the Mojave: each chunk around the player is decorated once, as soon as
 * New Vegas has described the ground under it and its neighbours (NvLand). The ground's texture
 * picks the biome (sand: desert; dirt: savanna; grass: plains; rock and steep slopes: windswept
 * hills; snow: snowy plains), whose vanilla features grow on the columns no New Vegas object,
 * road or water covers. Each feature stands on a "root" block of the ground's own material, so it
 * finds the soil it needs and its foot meets New Vegas' ground (the root sits within half a block
 * of the surface). Real blocks: they break, drop and are saved like any other.
 */
public final class NvDecorator {
	/** The decoration version a chunk got (0: none yet). Saved with the chunk. */
	public static final AttachmentType<Integer> DECORATED = AttachmentRegistry.<Integer>builder()
		.persistent(Codec.INT)
		.buildAndRegister(Identifier.fromNamespaceAndPath(SkyCraft.MOD_ID, "decorated"));
	private static final int VERSION = 1;
	private static final int RADIUS = 5;          // chunks around each player (NvLand covers 6)
	private static final int CHUNKS_PER_TICK = 1;

	public enum Kind { DESERT, SAVANNA, PLAINS, HILLS, SNOWY }

	/** A feature tried `tries` times per chunk, each with this chance. */
	private record Growth(String feature, int tries, float chance, Shape shape) {
	}

	private enum Shape {
		PLANT(0, 1.0),  // one block: needs its own column only
		BUSH(1, 1.5),   // a cactus, a boulder
		TREE(3, 1.5);   // a canopy that would poke into New Vegas buildings
		final int clearance;
		final double maxSlope;

		Shape(int clearance, double maxSlope) {
			this.clearance = clearance;
			this.maxSlope = maxSlope;
		}
	}

	private static final List<Growth> DESERT = List.of(
		new Growth("cactus", 2, 0.35F, Shape.BUSH),
		new Growth("dead_bush", 3, 0.6F, Shape.PLANT),
		new Growth("dry_grass", 4, 0.6F, Shape.PLANT));
	private static final List<Growth> SAVANNA = List.of(
		new Growth("trees_savanna", 1, 0.3F, Shape.TREE),
		new Growth("dry_grass", 5, 0.7F, Shape.PLANT),
		new Growth("dead_bush", 2, 0.5F, Shape.PLANT),
		new Growth("grass", 2, 0.4F, Shape.PLANT));
	private static final List<Growth> PLAINS = List.of(
		new Growth("trees_plains", 1, 0.35F, Shape.TREE),
		new Growth("grass", 8, 0.8F, Shape.PLANT),
		new Growth("flower_plain", 2, 0.5F, Shape.PLANT));
	private static final List<Growth> HILLS = List.of(
		new Growth("forest_rock", 1, 0.35F, Shape.BUSH),
		new Growth("spruce", 1, 0.2F, Shape.TREE),
		new Growth("dry_grass", 2, 0.5F, Shape.PLANT));
	private static final List<Growth> SNOWY = List.of(
		new Growth("spruce", 1, 0.35F, Shape.TREE),
		new Growth("grass", 2, 0.3F, Shape.PLANT));

	private NvDecorator() {
	}

	public static void init() {
		ServerTickEvents.END_SERVER_TICK.register(NvDecorator::tick);
	}

	private static void tick(MinecraftServer server) {
		ServerLevel level = server.overworld();
		if (NvLand.size() == 0 || level.players().isEmpty()) {
			return;
		}
		int budget = CHUNKS_PER_TICK;
		for (ServerPlayer player : level.players()) {
			Integer world = NvWorlds.worldOf(player);
			if (world == null || world != NvWorlds.WASTELAND_NV) {
				continue;
			}
			ChunkPos centre = player.chunkPosition();
			// Nearest first.
			for (int r = 0; r <= RADIUS && budget > 0; r++) {
				for (int dx = -r; dx <= r && budget > 0; dx++) {
					for (int dz = -r; dz <= r && budget > 0; dz++) {
						if (Math.max(Math.abs(dx), Math.abs(dz)) != r) {
							continue;
						}
						if (tryDecorate(level, centre.x() + dx, centre.z() + dz)) {
							budget--;
						}
					}
				}
			}
		}
	}

	/** Decorates the chunk if it's due and everything around it is known. */
	private static boolean tryDecorate(ServerLevel level, int cx, int cz) {
		LevelChunk chunk = level.getChunkSource().getChunkNow(cx, cz);
		if (chunk == null) {
			return false;
		}
		Integer done = chunk.getAttached(DECORATED);
		if (done != null && done >= VERSION) {
			return false;
		}
		NvLand.Chunk land = NvLand.get(new ChunkPos(cx, cz));
		if (land == null || !land.complete() || land.worldId() != NvWorlds.WASTELAND_NV) {
			return false;
		}
		// Trees and structures reach into the neighbours: those must be loaded and known too.
		for (int dx = -1; dx <= 1; dx++) {
			for (int dz = -1; dz <= 1; dz++) {
				NvLand.Chunk n = NvLand.get(new ChunkPos(cx + dx, cz + dz));
				if (level.getChunkSource().getChunkNow(cx + dx, cz + dz) == null || n == null || !n.complete()) {
					return false;
				}
			}
		}
		chunk.setAttached(DECORATED, VERSION);
		try {
			decorate(level, chunk, land);
		} catch (RuntimeException e) {
			SkyCraft.LOG.warn("SkyCraft: couldn't decorate chunk {} {}", cx, cz, e);
		}
		return true;
	}

	private static void decorate(ServerLevel level, LevelChunk chunk, NvLand.Chunk land) {
		ChunkPos pos = chunk.getPos();
		RandomSource random = RandomSource.create(level.getSeed() ^ (pos.pack() * 0x9E3779B97F4A7C15L));
		Kind kind = kindOf(level.getSeed(), pos, land);
		setBiome(level, pos, land, kind);
		int placed = 0;
		for (Growth g : growthsOf(kind)) {
			Holder<Feature> feature = feature(level, g.feature());
			if (feature == null) {
				continue;
			}
			for (int i = 0; i < g.tries(); i++) {
				if (random.nextFloat() >= g.chance()) {
					continue;
				}
				int x = pos.getMinBlockX() + random.nextInt(16), z = pos.getMinBlockZ() + random.nextInt(16);
				BlockPos root = rootAt(x, z, g.shape());
				if (root == null) {
					continue;
				}
				if (grow(level, feature, g.feature(), root, random)) {
					placed++;
				}
			}
		}
		if (kind == Kind.HILLS && random.nextFloat() < 0.12F) {
			placed += outcrop(level, pos, random);
		}
		placed += NvStructures.maybePlace(level, pos, kind, random);
		SkyCraft.LOG.debug("SkyCraft: decorated chunk {} {} as {} ({} features)", pos.x(), pos.z(), kind, placed);
	}

	/** The biome of a chunk: its commonest open ground, steepness, and a coarse noise for variety. */
	public static Kind kindOf(long seed, ChunkPos pos, NvLand.Chunk land) {
		int[] count = new int[DIG_MATERIAL_COUNT];
		float lo = Float.MAX_VALUE, hi = -Float.MAX_VALUE;
		for (int z = 0; z < 16; z++) {
			for (int x = 0; x < 16; x++) {
				if (!land.known(x, z)) {
					continue;
				}
				count[Math.min(land.material(x, z), DIG_MATERIAL_COUNT - 1)]++;
				lo = Math.min(lo, land.height(x, z));
				hi = Math.max(hi, land.height(x, z));
			}
		}
		int best = DIG_DIRT;
		for (int m = 0; m < count.length; m++) {
			if (count[m] > count[best]) {
				best = m;
			}
		}
		// Patches of four by four chunks share a roll, so biomes come in areas, not speckles.
		long patch = mix(seed ^ ((long) (pos.x() >> 2) * 341873128712L) ^ ((long) (pos.z() >> 2) * 132897987541L));
		boolean alt = (patch & 0xFF) < 90;
		if (hi - lo > 10.0F) {
			return Kind.HILLS;
		}
		return switch (best) {
			case DIG_SAND -> alt ? Kind.SAVANNA : Kind.DESERT;
			case DIG_GRASS -> alt ? Kind.SAVANNA : Kind.PLAINS;
			case DIG_STONE, DIG_COBBLE, DIG_GRAVEL -> alt ? Kind.DESERT : Kind.HILLS;
			case DIG_SNOW, DIG_ICE -> Kind.SNOWY;
			default -> alt ? Kind.DESERT : Kind.SAVANNA;  // the Mojave's dirt
		};
	}

	private static List<Growth> growthsOf(Kind kind) {
		return switch (kind) {
			case DESERT -> DESERT;
			case SAVANNA -> SAVANNA;
			case PLAINS -> PLAINS;
			case HILLS -> HILLS;
			case SNOWY -> SNOWY;
		};
	}

	public static ResourceKey<Biome> biomeOf(Kind kind) {
		return switch (kind) {
			case DESERT -> Biomes.DESERT;
			case SAVANNA -> Biomes.SAVANNA;
			case PLAINS -> Biomes.PLAINS;
			case HILLS -> Biomes.WINDSWEPT_HILLS;
			case SNOWY -> Biomes.SNOWY_PLAINS;
		};
	}

	/** The chunk's biome around its ground (grass and leaf colours, mob spawns), sent to the clients. */
	private static void setBiome(ServerLevel level, ChunkPos pos, NvLand.Chunk land, Kind kind) {
		var biome = level.registryAccess().lookupOrThrow(Registries.BIOME).get(biomeOf(kind));
		if (biome.isEmpty()) {
			return;
		}
		float lo = Float.MAX_VALUE, hi = -Float.MAX_VALUE;
		for (float h : land.height()) {
			lo = Math.min(lo, h);
			hi = Math.max(hi, h);
		}
		int y0 = Math.max(level.getMinY(), (int) Math.floor(lo) - 8), y1 = Math.min(level.getMaxY(), (int) Math.ceil(hi) + 24);
		FillBiomeCommand.fill(level, new BlockPos(pos.getMinBlockX(), y0, pos.getMinBlockZ()), new BlockPos(pos.getMinBlockX() + 15, y1, pos.getMinBlockZ() + 15), biome.get());
	}

	private static @Nullable Holder<Feature> feature(ServerLevel level, String name) {
		var key = ResourceKey.create(Registries.FEATURE, Identifier.withDefaultNamespace(name));
		var holder = level.registryAccess().lookupOrThrow(Registries.FEATURE).get(key);
		if (holder.isEmpty()) {
			SkyCraft.LOG.warn("SkyCraft: no feature minecraft:{}", name);
			return null;
		}
		return holder.get();
	}

	/**
	 * The root block for a feature at column (x, z): within half a block of New Vegas' surface, on
	 * open ground whose slope and surroundings suit the feature. Null where it can't grow.
	 */
	private static @Nullable BlockPos rootAt(int x, int z, Shape shape) {
		NvLand.Chunk here = NvLand.at(x, z);
		if (here == null || !here.open(x, z)) {
			return null;
		}
		double h = here.height(x, z), lo = h, hi = h;
		int c = shape.clearance;
		for (int dz = -Math.max(c, 1); dz <= Math.max(c, 1); dz++) {
			for (int dx = -Math.max(c, 1); dx <= Math.max(c, 1); dx++) {
				NvLand.Chunk n = NvLand.at(x + dx, z + dz);
				if (n == null || !n.known(x + dx, z + dz)) {
					return null;
				}
				boolean near = Math.abs(dx) <= 1 && Math.abs(dz) <= 1;
				if (Math.abs(dx) <= c && Math.abs(dz) <= c && (n.flags(x + dx, z + dz) & (LAND_BLOCKED | LAND_WATER)) != 0) {
					return null;
				}
				if (near) {
					double nh = n.height(x + dx, z + dz);
					lo = Math.min(lo, nh);
					hi = Math.max(hi, nh);
				}
			}
		}
		if (hi - lo > shape.maxSlope) {
			return null;
		}
		return new BlockPos(x, (int) Math.floor(h - 0.5), z);
	}

	/** The ground block New Vegas' texture at this column stands for. */
	static BlockState groundState(int x, int z) {
		NvLand.Chunk c = NvLand.at(x, z);
		int material = c != null ? c.material(x, z) : DIG_DIRT;
		return SkyDig.materialState(material == DIG_NONE ? DIG_STONE : material);
	}

	private static boolean grow(ServerLevel level, Holder<Feature> feature, String name, BlockPos root, RandomSource random) {
		BlockPos above = root.above();
		if (!level.getBlockState(root).isAir() || !level.getBlockState(above).isAir()) {
			return false;
		}
		BlockState ground = groundState(root.getX(), root.getZ());
		if (name.equals("cactus") || name.equals("dead_bush")) {
			ground = Blocks.SAND.defaultBlockState();  // what they grow on
		}
		level.setBlock(root, ground, 2);
		if (feature.value().place(level, level.getChunkSource().getGenerator(), random, above)) {
			return true;
		}
		level.setBlock(root, Blocks.AIR.defaultBlockState(), 2);
		return false;
	}

	/** A few rock and ore blocks breaking through the ground of a stony chunk. */
	private static int outcrop(ServerLevel level, ChunkPos pos, RandomSource random) {
		int x = pos.getMinBlockX() + 2 + random.nextInt(12), z = pos.getMinBlockZ() + 2 + random.nextInt(12);
		BlockPos root = rootAt(x, z, Shape.BUSH);
		if (root == null) {
			return 0;
		}
		BlockState[] ores = { Blocks.COAL_ORE.defaultBlockState(), Blocks.IRON_ORE.defaultBlockState(), Blocks.COPPER_ORE.defaultBlockState(), Blocks.GOLD_ORE.defaultBlockState() };
		BlockState ore = ores[random.nextInt(random.nextInt(4) + 1)];  // gold is rarer
		int n = 0;
		for (int i = 0; i < 6; i++) {
			BlockPos p = root.offset(random.nextInt(3) - 1, random.nextInt(2), random.nextInt(3) - 1);
			double h = NvLand.height(p.getX(), p.getZ());
			if (Double.isNaN(h) || p.getY() > h + 0.5 || !level.getBlockState(p).isAir()) {
				continue;
			}
			level.setBlock(p, random.nextInt(3) == 0 ? ore : Blocks.STONE.defaultBlockState(), 2);
			n++;
		}
		return n > 0 ? 1 : 0;
	}

	static long mix(long z) {
		z = (z ^ (z >>> 30)) * 0xBF58476D1CE4E5B9L;
		z = (z ^ (z >>> 27)) * 0x94D049BB133111EBL;
		return z ^ (z >>> 31);
	}
}
