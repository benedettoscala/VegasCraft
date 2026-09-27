package dev.vegascraft.item;

import static dev.vegascraft.link.Proto.*;
import static java.lang.foreign.ValueLayout.JAVA_BYTE;
import static java.lang.foreign.ValueLayout.JAVA_FLOAT;
import static java.lang.foreign.ValueLayout.JAVA_INT;
import static java.lang.foreign.ValueLayout.JAVA_SHORT;

import dev.vegascraft.SkyCraft;
import dev.vegascraft.link.SkyLink;
import java.lang.foreign.MemorySegment;
import java.nio.charset.StandardCharsets;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
import net.fabricmc.fabric.api.event.lifecycle.v1.ServerEntityEvents;
import net.fabricmc.fabric.api.event.lifecycle.v1.ServerTickEvents;
import net.fabricmc.fabric.api.event.player.UseItemCallback;
import net.minecraft.world.InteractionResult;
import net.minecraft.core.component.DataComponents;
import net.minecraft.nbt.CompoundTag;
import net.minecraft.resources.Identifier;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.entity.item.ItemEntity;
import net.minecraft.world.entity.player.Inventory;
import net.minecraft.world.item.ItemStack;
import net.minecraft.world.item.component.CustomData;
import net.minecraft.world.item.component.ItemLore;

/**
 * New Vegas owns its items; Minecraft's inventory holds only Minecraft's, plus links to New Vegas
 * items in the hotbar. New Vegas sends its whole inventory (kColInventory) when it changes: the
 * item New Vegas has on hotkey 1-8 is linked in hotbar slot 1-8 (only into a free slot: a Minecraft item there stays), and every link shows New Vegas' count, or goes when New Vegas no longer has the
 * item. A link wields its weapon, uses its aid (kEvNvUse) and, thrown away, makes New Vegas drop
 * the item in its world (kEvNvDrop). Links never go into Minecraft containers ({@code SlotMixin}).
 */
public final class NvInventory {
	/** A snapshot older than this means New Vegas stopped sending (closed, or out of a game). */
	private static final long STALE_MS = 10_000;
	/** A drop New Vegas hasn't confirmed by then is forgotten (it refused: a quest item). */
	private static final long DROP_WAIT_MS = 5_000;
	private static final int MAX_STACK = 99;

	private record Snapshot(int seq, int worldId, Map<Integer, NvItems.Taken> items, long receivedMs) {
	}

	private record Pending(int count, int nvBefore, long untilMs) {
	}

	private static volatile Snapshot snapshot;
	private static final Map<Integer, Pending> PENDING_DROPS = new ConcurrentHashMap<>();
	private static int lastLoggedKinds = -1;

	private NvInventory() {
	}

	public static void init() {
		ServerTickEvents.END_SERVER_TICK.register(NvInventory::serverTick);
		// Using a linked aid item (right click) uses it in New Vegas, which then counts one less.
		UseItemCallback.EVENT.register((player, level, hand) -> {
			ItemStack stack = player.getItemInHand(hand);
			CompoundTag tag = NvItems.data(stack);
			if (tag == null || level.isClientSide() || tag.getIntOr("kind", -1) != ITEM_AID) {
				return InteractionResult.PASS;
			}
			SkyLink.pushEvent(EV_NV_USE, tag.getIntOr("id", 0), 1.0F, 0.0F, 0.0F, 0.0F, 0);
			player.getCooldowns().addCooldown(stack, 10);
			return InteractionResult.SUCCESS;
		});
		ServerEntityEvents.ENTITY_LOAD.register((entity, level) -> {
			if (entity instanceof ItemEntity item && NvItems.data(item.getItem()) != null) {
				level.getServer().execute(() -> thrown(level.getServer(), item));
			}
		});
	}

	/** Collision consumer thread: an InventoryHeader + InventoryEntry[count] payload. */
	public static void read(MemorySegment s, long p, int payloadBytes) {
		if (payloadBytes < INV_HEADER_BYTES) {
			return;
		}
		int seq = s.get(JAVA_INT, p);
		int count = Math.min(s.get(JAVA_INT, p + 4), INV_MAX_ENTRIES);
		int worldId = s.get(JAVA_INT, p + 8);
		count = Math.min(count, (payloadBytes - INV_HEADER_BYTES) / INV_ENTRY_BYTES);
		Map<Integer, NvItems.Taken> items = new LinkedHashMap<>();
		for (int i = 0; i < count; i++) {
			long e = p + INV_HEADER_BYTES + (long) i * INV_ENTRY_BYTES;
			int form = s.get(JAVA_INT, e + IE_FORM);
			int flags = s.get(JAVA_BYTE, e + IE_FLAGS) & 0xFF;
			items.put(form, new NvItems.Taken(s.get(JAVA_SHORT, e + IE_KIND) & 0xFFFF, form, s.get(JAVA_INT, e + IE_COUNT), s.get(JAVA_FLOAT, e + IE_DAMAGE),
				text(s, e + IE_NAME, IE_NAME_BYTES), s.get(JAVA_BYTE, e + IE_CLASS) & 0xFF, s.get(JAVA_INT, e + IE_VALUE), s.get(JAVA_FLOAT, e + IE_WEIGHT),
				s.get(JAVA_INT, e + IE_HEALTH), text(s, e + IE_ICON, IE_ICON_BYTES), (flags & INV_EQUIPPED) != 0, (flags >> INV_HOTKEY_SHIFT) - 1));
		}
		snapshot = new Snapshot(seq, worldId, items, System.currentTimeMillis());
	}

	private static String text(MemorySegment s, long at, int max) {
		byte[] bytes = new byte[max];
		int n = 0;
		while (n < max && (bytes[n] = s.get(JAVA_BYTE, at + n)) != 0) {
			n++;
		}
		return new String(bytes, 0, n, StandardCharsets.ISO_8859_1);
	}

	private static void serverTick(MinecraftServer server) {
		if (server.getTickCount() % 5 != 0) {
			return;
		}
		for (ServerPlayer player : server.getPlayerList().getPlayers()) {
			if (server.isSingleplayerOwner(player.nameAndId())) {
				reconcile(player);
			}
		}
	}

	/** New Vegas' inventory as last sent (empty before the first snapshot), for the Pip-Boy's list. */
	public static java.util.Collection<NvItems.Taken> items() {
		Snapshot snap = snapshot;
		return snap == null ? java.util.List.of() : snap.items().values();
	}

	/**
	 * The Pip-Boy's list linked `form` in hotbar slot `slot` (a Minecraft item there moves to a free
	 * slot), or, for form 0, took the slot's link away. New Vegas' hotkeys still own slots 1-8.
	 */
	public static void link(ServerPlayer player, int slot, int form) {
		Snapshot snap = snapshot;
		if (snap == null || slot < 0 || slot >= Inventory.SELECTION_SIZE) {
			return;
		}
		Inventory inventory = player.getInventory();
		ItemStack there = inventory.getItem(slot);
		if (form == 0) {
			if (NvItems.data(there) != null) {
				inventory.setItem(slot, ItemStack.EMPTY);
			}
			return;
		}
		NvItems.Taken t = snap.items().get(form);
		if (t == null || shown(t) <= 0) {
			return;
		}
		if (!there.isEmpty() && NvItems.data(there) == null && !moveAway(inventory, slot)) {
			player.sendSystemMessage(net.minecraft.network.chat.Component.literal("No room to move " + there.getHoverName().getString() + " out of the hotbar"));
			return;
		}
		inventory.setItem(slot, NvItems.create(t, shown(t), -1));
		inventory.setChanged();
	}

	/** What a link shows: New Vegas' count, less what was just thrown away, within one stack. */
	private static int shown(NvItems.Taken t) {
		Pending pending = PENDING_DROPS.get(t.formId());
		int nv = t.count() - (pending == null ? 0 : pending.count());
		boolean single = t.kind() == ITEM_WEAPON || t.kind() == ITEM_ARMOR;
		return Math.max(0, Math.min(nv, single ? 1 : MAX_STACK));
	}

	private static int form(ItemStack stack) {
		CompoundTag tag = NvItems.data(stack);
		return tag == null ? 0 : tag.getIntOr("id", 0);
	}

	/** A link that should stay: New Vegas still has the item (and, for a hotkey's, the same hotkey). */
	private static boolean valid(ItemStack stack, Snapshot snap) {
		CompoundTag tag = NvItems.data(stack);
		NvItems.Taken t = tag == null ? null : snap.items().get(tag.getIntOr("id", 0));
		if (t == null || !tag.getBooleanOr("link", false) || shown(t) <= 0) {
			return false;
		}
		int hotkey = tag.getIntOr("hotkey", -1);
		return hotkey < 0 || hotkey == t.hotkey();
	}

	public static void reconcile(ServerPlayer player) {
		Snapshot snap = snapshot;
		long now = System.currentTimeMillis();
		if (snap == null || snap.worldId() == 0 || now - snap.receivedMs() > STALE_MS) {
			return;
		}
		if (snap.items().size() != lastLoggedKinds) {
			lastLoggedKinds = snap.items().size();
			SkyCraft.LOG.info("VegasCraft: New Vegas inventory #{}: {} kinds of items", snap.seq(), snap.items().size());
		}
		PENDING_DROPS.entrySet().removeIf(e -> {
			NvItems.Taken t = snap.items().get(e.getKey());
			int nv = t == null ? 0 : t.count();
			return nv <= e.getValue().nvBefore() - e.getValue().count() || now > e.getValue().untilMs();
		});
		Inventory inventory = player.getInventory();
		boolean changed = false;
		// Links stay up to date; anything else carrying New Vegas' tag (a stale link, a copy from an
		// older version) goes: New Vegas' own list holds every item.
		for (int i = 0; i < inventory.getContainerSize(); i++) {
			ItemStack stack = inventory.getItem(i);
			if (NvItems.data(stack) == null) {
				continue;
			}
			if (!valid(stack, snap)) {
				inventory.setItem(i, ItemStack.EMPTY);
				changed = true;
			} else {
				changed |= refresh(stack, snap.items().get(form(stack)));
			}
		}
		ItemStack carried = player.containerMenu.getCarried();
		if (NvItems.data(carried) != null && !valid(carried, snap)) {
			player.containerMenu.setCarried(ItemStack.EMPTY);
		}
		// New Vegas' hotkeys 1-8 are the hotbar's first eight slots.
		for (NvItems.Taken t : snap.items().values()) {
			int slot = t.hotkey();
			if (slot < 0 || slot >= 8 || shown(t) <= 0) {
				continue;
			}
			ItemStack there = inventory.getItem(slot);
			if (form(there) == t.formId() && NvItems.data(there).getIntOr("hotkey", -1) == slot) {
				continue;
			}
			// The hotbar is the player's: a hotkey fills a free slot only, never takes one a Minecraft item
			// sits in, and doesn't link again an item the player already moved to another slot.
			if (!there.isEmpty() || linkedInHotbar(inventory, t.formId())) {
				continue;
			}
			inventory.setItem(slot, NvItems.create(t, shown(t), slot));
			changed = true;
		}
		if (changed) {
			inventory.setChanged();
		}
	}

	private static boolean linkedInHotbar(Inventory inventory, int form) {
		for (int i = 0; i < inventory.getContainerSize(); i++) {
			if (form(inventory.getItem(i)) == form) {
				return true;
			}
		}
		return false;
	}

	/** Count, image and tooltip of a link, as New Vegas has the item now. */
	private static boolean refresh(ItemStack stack, NvItems.Taken t) {
		boolean changed = false;
		int count = shown(t);
		if (stack.getCount() != count) {
			stack.setCount(count);
			changed = true;
		}
		CompoundTag tag = NvItems.data(stack);
		String model = NvItems.model(t);
		if (tag.getIntOr("nvcount", -1) != t.count() || !model.equals(String.valueOf(stack.get(DataComponents.ITEM_MODEL)))) {
			CompoundTag outer = stack.getOrDefault(DataComponents.CUSTOM_DATA, CustomData.EMPTY).copyTag();
			CompoundTag inner = outer.getCompoundOrEmpty(NvItems.TAG);
			inner.putInt("nvcount", t.count());
			inner.putString("icon", t.icon());
			outer.put(NvItems.TAG, inner);
			stack.set(DataComponents.CUSTOM_DATA, CustomData.of(outer));
			stack.set(DataComponents.ITEM_MODEL, Identifier.parse(model));
			stack.set(DataComponents.LORE, new ItemLore(NvItems.lore(t)));
			changed = true;
		}
		return changed;
	}

	/** Moves the Minecraft item in a hotbar slot to a free slot (the main inventory first). */
	private static boolean moveAway(Inventory inventory, int slot) {
		int free = -1;
		for (int i = Inventory.SELECTION_SIZE; i < Inventory.INVENTORY_SIZE && free < 0; i++) {
			if (inventory.getItem(i).isEmpty()) {
				free = i;
			}
		}
		for (int i = 0; i < Inventory.SELECTION_SIZE && free < 0; i++) {
			if (i != slot && inventory.getItem(i).isEmpty()) {
				free = i;
			}
		}
		if (free < 0) {
			return false;
		}
		inventory.setItem(free, inventory.getItem(slot));
		inventory.setItem(slot, ItemStack.EMPTY);
		return true;
	}

	/**
	 * A New Vegas copy appeared in the world: thrown away by the host (New Vegas drops the item in
	 * its own world instead), or anything else (a leftover from an older save): it never stays.
	 */
	private static void thrown(MinecraftServer server, ItemEntity item) {
		if (item.isRemoved()) {
			return;
		}
		ItemStack stack = item.getItem();
		int form = form(stack);
		int count = stack.getCount();
		boolean host = item.getOwner() instanceof ServerPlayer player && server.isSingleplayerOwner(player.nameAndId());
		item.discard();
		if (!host || form == 0) {
			return;
		}
		Snapshot snap = snapshot;
		NvItems.Taken t = snap == null ? null : snap.items().get(form);
		int nvBefore = t == null ? count : t.count();
		Pending previous = PENDING_DROPS.get(form);
		int pending = count + (previous == null ? 0 : previous.count());
		PENDING_DROPS.put(form, new Pending(pending, previous == null ? nvBefore : previous.nvBefore(), System.currentTimeMillis() + DROP_WAIT_MS));
		SkyLink.pushEvent(EV_NV_DROP, form, count, 0.0F, 0.0F, 0.0F, 0);
		SkyCraft.LOG.info("VegasCraft: threw {} x{} away: New Vegas drops it", String.format("%08X", form), count);
	}
}
