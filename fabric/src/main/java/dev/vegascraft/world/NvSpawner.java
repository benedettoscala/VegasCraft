package dev.vegascraft.world;

import dev.vegascraft.SkyCraft;
import dev.vegascraft.link.SkyLink;
import java.util.List;
import net.fabricmc.fabric.api.event.lifecycle.v1.ServerTickEvents;
import net.minecraft.core.BlockPos;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.util.RandomSource;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.entity.EntitySpawnReason;
import net.minecraft.world.entity.EntityType;
import net.minecraft.world.entity.EntityTypes;
import net.minecraft.world.entity.Mob;
import net.minecraft.world.entity.monster.Enemy;
import net.minecraft.world.level.ChunkPos;
import net.minecraft.world.phys.AABB;
import net.minecraft.world.phys.Vec3;

/**
 * Minecraft's animals by day and monsters by night on the Mojave. Vanilla spawning needs solid
 * blocks to stand on, so mobs are put on New Vegas' ground (NvLand) instead, inside the range
 * where Minecraft has New Vegas' collision (so they walk on it). A mob that ends up beyond that
 * range is held still on the ground until the player is back, and despawns far away. Minecraft's
 * time of day follows New Vegas' clock, so the sun burns zombies at New Vegas' dawn.
 */
public final class NvSpawner {
	private static final String TAG = "vegascraft_nv";
	private static final int PERIOD = 40;              // ticks between spawn attempts
	private static final int ANIMALS = 4, MONSTERS = 4; // per player
	private static final double MIN_DIST = 24, MAX_DIST = 36, DESPAWN = 96, COUNT_RANGE = 64;
	private static final SkyLink.SkyState SKY = new SkyLink.SkyState();
	private static final int[] FAILS = new int[3];
	private static long lastTimeSync = Long.MIN_VALUE;

	private NvSpawner() {
	}

	public static void init() {
		ServerTickEvents.END_SERVER_TICK.register(NvSpawner::tick);
	}

	/**
	 * config/vegascraft.properties {@code mobs=false} turns the spawner off (re-read every 10 s, no
	 * restart needed); the mobs it spawned go too.
	 */
	private static boolean enabled(MinecraftServer server) {
		long now = System.currentTimeMillis();
		if (now - enabledReadAt > 10_000L) {
			enabledReadAt = now;
			java.util.Properties props = new java.util.Properties();
			java.nio.file.Path file = net.fabricmc.loader.api.FabricLoader.getInstance().getConfigDir().resolve("vegascraft.properties");
			try (var in = java.nio.file.Files.newBufferedReader(file)) {
				props.load(in);
			} catch (java.io.IOException ignored) {
				// no file: mobs on
			}
			boolean before = enabled;
			enabled = !"false".equalsIgnoreCase(props.getProperty("mobs", "true").trim());
			if (before != enabled) {
				SkyCraft.LOG.info("SkyCraft: Minecraft mobs on the Mojave {}", enabled ? "on" : "off (mobs=false)");
			}
		}
		return enabled;
	}

	private static boolean enabled = true;
	private static long enabledReadAt = Long.MIN_VALUE / 2;

	private static void tick(MinecraftServer server) {
		ServerLevel level = server.overworld();
		if (!enabled(server)) {
			if (server.getTickCount() % 20 == 0) {
				java.util.List<Mob> ours = new java.util.ArrayList<>();
				for (ServerLevel l : server.getAllLevels()) {
					for (Entity e : l.getAllEntities()) {
						if (e instanceof Mob m && m.entityTags().contains(TAG)) {
							ours.add(m);
						}
					}
				}
				ours.forEach(Mob::discard);
			}
			return;
		}
		if (server.getTickCount() % 5 == 0) {
			holdOutOfRange(level);
		}
		if (server.getTickCount() % PERIOD != 0 || !SkyLink.readSkyState(SKY) || !SKY.inGame()) {
			return;
		}
		syncTime(server);
		if (NvLand.size() == 0) {
			return;
		}
		boolean night = SKY.gameHour < 6.0F || SKY.gameHour >= 20.0F;
		if (server.getTickCount() % 6000 == 0) {
			SkyCraft.LOG.info("SkyCraft: spawner at New Vegas hour {} ({}), {} land chunks; missed: {} covered, {} no collision, {} stuck", SKY.gameHour, night ? "night" : "day", NvLand.size(), FAILS[0], FAILS[1], FAILS[2]);
		}
		for (ServerPlayer player : level.players()) {
			Integer world = NvWorlds.worldOf(player);
			if (world == null || world != NvWorlds.WASTELAND_NV || player.isSpectator()) {
				continue;
			}
			AABB around = player.getBoundingBox().inflate(COUNT_RANGE);
			int animals = level.getEntitiesOfClass(Mob.class, around, m -> m.entityTags().contains(TAG) && !(m instanceof Enemy)).size();
			int monsters = level.getEntitiesOfClass(Mob.class, around, m -> m.entityTags().contains(TAG) && m instanceof Enemy).size();
			RandomSource random = level.getRandom();
			if (night && monsters < MONSTERS) {
				trySpawn(level, player, random, true);
			} else if (!night && animals < ANIMALS && random.nextInt(3) == 0) {
				trySpawn(level, player, random, false);
			}
		}
	}

	/** Minecraft's clock at New Vegas' hour (Minecraft's day starts at 6:00). */
	private static void syncTime(MinecraftServer server) {
		long ticks = (long) ((((SKY.gameHour - 6.0F) % 24.0F + 24.0F) % 24.0F) * 1000.0F);
		long now = server.overworld().getOverworldClockTime() % 24000L;
		long diff = Math.abs(ticks - now);
		if (Math.min(diff, 24000L - diff) < 200 && lastTimeSync != Long.MIN_VALUE) {
			return;
		}
		lastTimeSync = ticks;
		server.getCommands().performPrefixedCommand(server.createCommandSourceStack().withSuppressedOutput(), "time set " + ticks);
	}

	private static List<EntityType<? extends Mob>> species(NvDecorator.Kind kind, boolean monster) {
		if (monster) {
			return switch (kind) {
				case DESERT -> List.of(EntityTypes.HUSK, EntityTypes.HUSK, EntityTypes.SKELETON, EntityTypes.SPIDER, EntityTypes.CREEPER);
				case SNOWY -> List.of(EntityTypes.STRAY, EntityTypes.ZOMBIE, EntityTypes.SPIDER, EntityTypes.CREEPER);
				default -> List.of(EntityTypes.ZOMBIE, EntityTypes.ZOMBIE, EntityTypes.SKELETON, EntityTypes.SPIDER, EntityTypes.CREEPER);
			};
		}
		return switch (kind) {
			case DESERT -> List.of(EntityTypes.RABBIT, EntityTypes.CAMEL);
			case SAVANNA -> List.of(EntityTypes.HORSE, EntityTypes.COW, EntityTypes.SHEEP, EntityTypes.CHICKEN);
			case PLAINS -> List.of(EntityTypes.COW, EntityTypes.PIG, EntityTypes.SHEEP, EntityTypes.CHICKEN);
			case HILLS -> List.of(EntityTypes.GOAT, EntityTypes.SHEEP, EntityTypes.LLAMA);
			case SNOWY -> List.of(EntityTypes.RABBIT, EntityTypes.POLAR_BEAR);
		};
	}

	private static void trySpawn(ServerLevel level, ServerPlayer player, RandomSource random, boolean monster) {
		double angle = random.nextDouble() * Math.PI * 2, dist = MIN_DIST + random.nextDouble() * (MAX_DIST - MIN_DIST);
		int x = (int) Math.floor(player.getX() + Math.cos(angle) * dist), z = (int) Math.floor(player.getZ() + Math.sin(angle) * dist);
		NvLand.Chunk land = NvLand.at(x, z);
		if (land == null || !land.known(x, z) || (land.flags(x, z) & (dev.vegascraft.link.Proto.LAND_BLOCKED | dev.vegascraft.link.Proto.LAND_WATER)) != 0) {
			FAILS[0]++;
			return;
		}
		double y = land.height(x, z) + 0.05;
		BlockPos feet = BlockPos.containing(x + 0.5, y, z + 0.5);
		if (!SkyCollision.isKnown(feet.getX(), feet.getY() - 1, feet.getZ()) || !level.getBlockState(feet).isAir() || !level.getBlockState(feet.above()).isAir()) {
			FAILS[1]++;
			return;  // no New Vegas collision there yet, or a block in the way
		}
		List<EntityType<? extends Mob>> types = species(NvDecorator.kindOf(level.getSeed(), new ChunkPos(x >> 4, z >> 4), land), monster);
		EntityType<? extends Mob> type = types.get(random.nextInt(types.size()));
		Mob mob = type.create(level, EntitySpawnReason.NATURAL);
		if (mob == null) {
			return;
		}
		// New Vegas' ground is 1/8-block voxels for mobs: lift it clear of them.
		float yaw = random.nextFloat() * 360.0F;
		boolean clear = false;
		for (int step = 0; step <= 8 && !clear; step++) {
			mob.snapTo(x + 0.5, y + step * 0.125, z + 0.5, yaw, 0.0F);
			clear = level.noCollision(mob);
		}
		if (!clear) {
			FAILS[2]++;
			return;
		}
		mob.finalizeSpawn(level, level.getCurrentDifficultyAt(feet), EntitySpawnReason.NATURAL, null);
		mob.addTag(TAG);
		level.addFreshEntity(mob);
		SkyCraft.LOG.debug("SkyCraft: spawned {} on New Vegas ground at {} {} {}", type, x, y, z);
	}

	/**
	 * Our mobs beyond Minecraft's copy of New Vegas' collision would fall through the ground:
	 * they stand still on it instead (NvLand), and despawn far from every player.
	 */
	private static void holdOutOfRange(ServerLevel level) {
		if (level.players().isEmpty()) {
			return;
		}
		for (Entity e : level.getAllEntities()) {
			if (!(e instanceof Mob mob) || !mob.entityTags().contains(TAG) || !mob.isAlive()) {
				continue;
			}
			Entity nearest = level.getNearestPlayer(mob, -1.0);
			if (nearest == null || nearest.distanceTo(mob) > DESPAWN) {
				mob.discard();
				continue;
			}
			BlockPos below = BlockPos.containing(mob.getX(), mob.getY() - 0.5, mob.getZ());
			boolean known = SkyCollision.isKnown(below.getX(), below.getY(), below.getZ());
			if (known) {
				if (mob.isNoGravity()) {
					mob.setNoGravity(false);
				}
				continue;
			}
			double h = NvLand.height(mob.getBlockX(), mob.getBlockZ());
			if (Double.isNaN(h)) {
				mob.discard();
				continue;
			}
			mob.setNoGravity(true);
			mob.setDeltaMovement(Vec3.ZERO);
			mob.getNavigation().stop();
			mob.snapTo(mob.getX(), h + 0.05, mob.getZ());
		}
	}
}
