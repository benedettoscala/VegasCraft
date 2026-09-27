package dev.vegascraft.world;

import dev.vegascraft.SkyCraft;
import java.util.Map;
import java.util.UUID;
import java.util.concurrent.ConcurrentHashMap;
import net.fabricmc.fabric.api.event.lifecycle.v1.ServerTickEvents;
import net.fabricmc.fabric.api.networking.v1.ServerPlayConnectionEvents;
import net.minecraft.core.registries.Registries;
import net.minecraft.resources.Identifier;
import net.minecraft.resources.ResourceKey;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.level.Level;
import net.minecraft.world.level.portal.TeleportTransition;
import net.minecraft.world.phys.Vec3;
import org.jspecify.annotations.Nullable;

/**
 * Minecraft's world shares coordinates with every New Vegas worldspace and interior, so blocks
 * generated on the Mojave would show up inside the Goodsprings saloon. The Mojave (WastelandNV)
 * is the overworld, the only dimension Minecraft's overworld features are generated in; every
 * other New Vegas world (interiors, the Strip, Freeside, the DLC worldspaces) shares the
 * "elsewhere" dimension, a void like the overworld used to be.
 */
public final class NvWorlds {
	/** WastelandNV's worldspace FormID (FalloutNV.esm). */
	public static final int WASTELAND_NV = 0x000DA726;
	public static final ResourceKey<Level> ELSEWHERE = ResourceKey.create(Registries.DIMENSION, Identifier.fromNamespaceAndPath(SkyCraft.MOD_ID, "elsewhere"));
	/** Each player's New Vegas world (SkyState.worldId), as their client reports it. */
	private static final Map<UUID, Integer> WORLDS = new ConcurrentHashMap<>();

	private NvWorlds() {
	}

	public static void init() {
		ServerTickEvents.END_SERVER_TICK.register(NvWorlds::tick);
		ServerPlayConnectionEvents.DISCONNECT.register((handler, server) -> WORLDS.remove(handler.getPlayer().getUUID()));
	}

	/** The dimension a New Vegas world maps to. */
	public static ResourceKey<Level> dimensionFor(int worldId) {
		return worldId == WASTELAND_NV ? Level.OVERWORLD : ELSEWHERE;
	}

	/** Is this the dimension of the Mojave (where Minecraft's features grow)? */
	public static boolean isMojave(Level level) {
		return level.dimension() == Level.OVERWORLD;
	}

	/** A client's New Vegas world (server thread). */
	public static void report(ServerPlayer player, int worldId) {
		if (worldId == 0) {
			return;
		}
		Integer before = WORLDS.put(player.getUUID(), worldId);
		if (before == null || before != worldId) {
			place(player);
		}
	}

	public static @Nullable Integer worldOf(ServerPlayer player) {
		return WORLDS.get(player.getUUID());
	}

	/** Every second: players that respawned or joined in the wrong dimension go to their own. */
	private static void tick(MinecraftServer server) {
		if (server.getTickCount() % 20 != 0) {
			return;
		}
		for (ServerPlayer player : server.getPlayerList().getPlayers()) {
			place(player);
		}
	}

	private static void place(ServerPlayer player) {
		Integer world = WORLDS.get(player.getUUID());
		if (world == null || !player.isAlive()) {
			return;
		}
		ResourceKey<Level> key = dimensionFor(world);
		if (player.level().dimension() == key) {
			return;
		}
		MinecraftServer server = player.level().getServer();
		ServerLevel target = server != null ? server.getLevel(key) : null;
		if (target == null) {
			SkyCraft.LOG.warn("SkyCraft: no dimension {} for New Vegas world {}", key.identifier(), Integer.toHexString(world));
			return;
		}
		// Same place in the other dimension: the client puts the new player where New Vegas' is.
		player.teleport(new TeleportTransition(target, player.position(), Vec3.ZERO, player.getYRot(), player.getXRot(), TeleportTransition.DO_NOTHING));
		SkyCraft.LOG.info("SkyCraft: {} is in New Vegas world {}: moved to {}", player.getName().getString(), Integer.toHexString(world), key.identifier());
	}
}
