package dev.vegascraft.net;

import dev.vegascraft.SkyCraft;
import dev.vegascraft.combat.SkyCombat;
import dev.vegascraft.world.SkyDig;
import java.util.List;
import net.minecraft.core.BlockPos;
import net.fabricmc.fabric.api.networking.v1.PayloadTypeRegistry;
import net.fabricmc.fabric.api.networking.v1.ServerPlayNetworking;
import net.minecraft.network.RegistryFriendlyByteBuf;
import net.minecraft.network.codec.ByteBufCodecs;
import net.minecraft.network.codec.StreamCodec;
import net.minecraft.network.protocol.common.custom.CustomPacketPayload;
import net.minecraft.resources.Identifier;
import net.minecraft.server.level.ServerPlayer;

/**
 * Multiplayer: every player has their own Skyrim, talking to their own Minecraft client. The host's
 * Skyrim reaches the host's integrated server through shared memory; a guest's Skyrim reaches the
 * host's server through these packets instead.
 */
public final class SkyNet {
	private SkyNet() {
	}

	/** Guest -> server: the guest's Skyrim hit them (as proto::InputEvent kInHurt). */
	public record Hurt(int kind, float skyrimDamage, int attackerFormId, int flags) implements CustomPacketPayload {
		public static final Type<Hurt> TYPE = new Type<>(Identifier.fromNamespaceAndPath(SkyCraft.MOD_ID, "hurt"));
		public static final StreamCodec<RegistryFriendlyByteBuf, Hurt> CODEC = StreamCodec.composite(
			ByteBufCodecs.VAR_INT, Hurt::kind,
			ByteBufCodecs.FLOAT, Hurt::skyrimDamage,
			ByteBufCodecs.INT, Hurt::attackerFormId,
			ByteBufCodecs.VAR_INT, Hurt::flags,
			Hurt::new
		);

		@Override
		public Type<? extends CustomPacketPayload> type() {
			return TYPE;
		}
	}

	/** Server -> guest: the guest died in Minecraft, so their Skyrim player dies too. */
	public record Died(int attackerFormId) implements CustomPacketPayload {
		public static final Type<Died> TYPE = new Type<>(Identifier.fromNamespaceAndPath(SkyCraft.MOD_ID, "died"));
		public static final StreamCodec<RegistryFriendlyByteBuf, Died> CODEC = StreamCodec.composite(ByteBufCodecs.INT, Died::attackerFormId, Died::new);

		@Override
		public Type<? extends CustomPacketPayload> type() {
			return TYPE;
		}
	}

	/** Client -> server: the player hit Skyrim's geometry in this cell (SkyDig.open). */
	public record DigOpen(int world, BlockPos pos, int material) implements CustomPacketPayload {
		public static final Type<DigOpen> TYPE = new Type<>(Identifier.fromNamespaceAndPath(SkyCraft.MOD_ID, "dig_open"));
		public static final StreamCodec<RegistryFriendlyByteBuf, DigOpen> CODEC = StreamCodec.composite(
			ByteBufCodecs.INT, DigOpen::world,
			BlockPos.STREAM_CODEC, DigOpen::pos,
			ByteBufCodecs.VAR_INT, DigOpen::material,
			DigOpen::new
		);

		@Override
		public Type<? extends CustomPacketPayload> type() {
			return TYPE;
		}
	}

	/** Client -> server: cells around a broken dug block that are inside Skyrim's geometry (SkyDig.reveal). */
	public record DigReveal(int world, List<BlockPos> cells, List<Integer> materials) implements CustomPacketPayload {
		public static final Type<DigReveal> TYPE = new Type<>(Identifier.fromNamespaceAndPath(SkyCraft.MOD_ID, "dig_reveal"));
		public static final StreamCodec<RegistryFriendlyByteBuf, DigReveal> CODEC = StreamCodec.composite(
			ByteBufCodecs.INT, DigReveal::world,
			BlockPos.STREAM_CODEC.apply(ByteBufCodecs.list(64)), DigReveal::cells,
			ByteBufCodecs.VAR_INT.apply(ByteBufCodecs.list(64)), DigReveal::materials,
			DigReveal::new
		);

		@Override
		public Type<? extends CustomPacketPayload> type() {
			return TYPE;
		}
	}

	/** Client -> server: the New Vegas world the player is in (SkyState.worldId; NvWorlds). */
	public record NvWorld(int worldId) implements CustomPacketPayload {
		public static final Type<NvWorld> TYPE = new Type<>(Identifier.fromNamespaceAndPath(SkyCraft.MOD_ID, "nv_world"));
		public static final StreamCodec<RegistryFriendlyByteBuf, NvWorld> CODEC = StreamCodec.composite(ByteBufCodecs.INT, NvWorld::worldId, NvWorld::new);

		@Override
		public Type<? extends CustomPacketPayload> type() {
			return TYPE;
		}
	}

	/** Client -> server: New Vegas fired the player's gun (kInNvShot; NvShots traces it in Minecraft). */
	public record NvShot(int weaponForm, float damage, int projectiles) implements CustomPacketPayload {
		public static final Type<NvShot> TYPE = new Type<>(Identifier.fromNamespaceAndPath(SkyCraft.MOD_ID, "nv_shot"));
		public static final StreamCodec<RegistryFriendlyByteBuf, NvShot> CODEC = StreamCodec.composite(
			ByteBufCodecs.INT, NvShot::weaponForm,
			ByteBufCodecs.FLOAT, NvShot::damage,
			ByteBufCodecs.VAR_INT, NvShot::projectiles,
			NvShot::new
		);

		@Override
		public Type<? extends CustomPacketPayload> type() {
			return TYPE;
		}
	}

	/** Client -> server: the Pip-Boy's Minecraft page opened (F4): a crafting table the player carries. */
	public record PipBoyCrafting() implements CustomPacketPayload {
		public static final PipBoyCrafting INSTANCE = new PipBoyCrafting();
		public static final Type<PipBoyCrafting> TYPE = new Type<>(Identifier.fromNamespaceAndPath(SkyCraft.MOD_ID, "pipboy_crafting"));
		public static final StreamCodec<RegistryFriendlyByteBuf, PipBoyCrafting> CODEC = StreamCodec.unit(INSTANCE);

		@Override
		public Type<? extends CustomPacketPayload> type() {
			return TYPE;
		}
	}

	/** Client -> server: the Pip-Boy's New Vegas list put an item in hotbar slot `slot` (form 0: took the link away). */
	public record NvLink(int slot, int form) implements CustomPacketPayload {
		public static final Type<NvLink> TYPE = new Type<>(Identifier.fromNamespaceAndPath(SkyCraft.MOD_ID, "nv_link"));
		public static final StreamCodec<RegistryFriendlyByteBuf, NvLink> CODEC = StreamCodec.composite(
			ByteBufCodecs.VAR_INT, NvLink::slot,
			ByteBufCodecs.INT, NvLink::form,
			NvLink::new
		);

		@Override
		public Type<? extends CustomPacketPayload> type() {
			return TYPE;
		}
	}

	public static void init() {
		PayloadTypeRegistry.serverboundPlay().register(NvLink.TYPE, NvLink.CODEC);
		ServerPlayNetworking.registerGlobalReceiver(NvLink.TYPE, (payload, context) -> {
			ServerPlayer player = context.player();
			context.server().execute(() -> dev.vegascraft.item.NvInventory.link(player, payload.slot(), payload.form()));
		});
		PayloadTypeRegistry.serverboundPlay().register(PipBoyCrafting.TYPE, PipBoyCrafting.CODEC);
		ServerPlayNetworking.registerGlobalReceiver(PipBoyCrafting.TYPE, (payload, context) -> {
			ServerPlayer player = context.player();
			// A crafting grid at the player's feet (the menu needs a level to find recipes and to hand the grid's items back
			// on close), with no table behind it: it stays valid wherever the player goes.
			context.server().execute(() -> player.openMenu(new net.minecraft.world.SimpleMenuProvider(
				(id, inventory, p) -> new net.minecraft.world.inventory.CraftingMenu(id, inventory,
					net.minecraft.world.inventory.ContainerLevelAccess.create(player.level(), player.blockPosition())) {
					@Override
					public boolean stillValid(net.minecraft.world.entity.player.Player who) {
						return true;
					}
				},
				net.minecraft.network.chat.Component.literal("Pip-Boy"))));
		});
		PayloadTypeRegistry.serverboundPlay().register(NvShot.TYPE, NvShot.CODEC);
		ServerPlayNetworking.registerGlobalReceiver(NvShot.TYPE, (payload, context) -> {
			ServerPlayer player = context.player();
			// A shot's worth of damage, whatever the client claims (as for Hurt).
			float damage = Math.max(0.0F, Math.min(payload.damage(), 2000.0F));
			int projectiles = Math.max(1, Math.min(payload.projectiles(), 30));
			context.server().execute(() -> dev.vegascraft.combat.NvShots.fire(player, payload.weaponForm(), damage, projectiles));
		});
		PayloadTypeRegistry.serverboundPlay().register(NvWorld.TYPE, NvWorld.CODEC);
		ServerPlayNetworking.registerGlobalReceiver(NvWorld.TYPE, (payload, context) -> {
			ServerPlayer player = context.player();
			context.server().execute(() -> dev.vegascraft.world.NvWorlds.report(player, payload.worldId()));
		});
		PayloadTypeRegistry.serverboundPlay().register(Hurt.TYPE, Hurt.CODEC);
		PayloadTypeRegistry.serverboundPlay().register(DigOpen.TYPE, DigOpen.CODEC);
		PayloadTypeRegistry.serverboundPlay().register(DigReveal.TYPE, DigReveal.CODEC);
		ServerPlayNetworking.registerGlobalReceiver(DigOpen.TYPE, (payload, context) -> {
			ServerPlayer player = context.player();
			context.server().execute(() -> SkyDig.open(player, payload.world(), payload.pos(), payload.material()));
		});
		ServerPlayNetworking.registerGlobalReceiver(DigReveal.TYPE, (payload, context) -> {
			ServerPlayer player = context.player();
			int[] materials = payload.materials().stream().mapToInt(Integer::intValue).toArray();
			context.server().execute(() -> SkyDig.reveal(player, payload.world(), payload.cells(), materials));
		});
		PayloadTypeRegistry.clientboundPlay().register(Died.TYPE, Died.CODEC);
		ServerPlayNetworking.registerGlobalReceiver(Hurt.TYPE, (payload, context) -> {
			ServerPlayer player = context.player();
			// A hit's worth of damage, whatever the guest's client claims (friends only, but still).
			float damage = Math.max(0.0F, Math.min(payload.skyrimDamage(), 10000.0F));
			context.server().execute(() -> SkyCombat.hurtPlayer(player, payload.kind(), damage, payload.attackerFormId(), payload.flags()));
		});
	}

	/** True if this player plays on this machine (their Skyrim is on the shared-memory link). */
	public static boolean isHost(ServerPlayer player) {
		var server = player.level().getServer();
		return server != null && server.isSingleplayerOwner(player.nameAndId());
	}
}
