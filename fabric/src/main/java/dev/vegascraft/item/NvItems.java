package dev.vegascraft.item;

import dev.vegascraft.link.Proto;
import java.util.ArrayList;
import java.util.List;
import net.minecraft.core.component.DataComponents;
import net.minecraft.nbt.CompoundTag;
import net.minecraft.network.chat.Component;
import net.minecraft.resources.Identifier;
import net.minecraft.world.entity.EquipmentSlotGroup;
import net.minecraft.world.entity.ai.attributes.AttributeModifier;
import net.minecraft.world.entity.ai.attributes.Attributes;
import net.minecraft.world.item.Item;
import net.minecraft.world.item.ItemStack;
import net.minecraft.world.item.component.ItemAttributeModifiers;
import net.minecraft.world.item.Items;
import net.minecraft.world.item.component.CustomData;
import net.minecraft.world.item.component.ItemLore;

/**
 * Fallout: New Vegas items inside Minecraft's inventory, mirrors of New Vegas' own inventory
 * ({@link NvInventory}). A stack is a plain stick with a name, New Vegas' inventory image (or a
 * borrowed vanilla model) and a {@code vegascraft_nv} tag (form id, kind, weapon class, damage,
 * image), so it needs no registry entry. A New Vegas weapon in the
 * main hand is wielded by New Vegas itself, which draws, fires and reloads it ({@link #weaponForm});
 * melee and unarmed weapons are Minecraft's to hold and swing ({@link #isMelee}).
 */
public final class NvItems {
	public static final String TAG = "vegascraft_nv";

	private NvItems() {
	}

	/** One kind of item in New Vegas' inventory (an InventoryEntry of a snapshot). */
	public record Taken(int kind, int formId, int count, float damage, String name, int weaponClass, int value, float weight, int health,
		String icon, boolean equipped, int hotkey) {
	}

	/** A link to `t` showing `count` (capped at the stack size), placed for New Vegas hotkey `hotkey` (-1: by hand). */
	public static ItemStack create(Taken t, int count, int hotkey) {
		boolean single = t.kind() == Proto.ITEM_WEAPON || t.kind() == Proto.ITEM_ARMOR;
		ItemStack stack = new ItemStack(Items.STICK, Math.max(1, Math.min(count, single ? 1 : 99)));
		boolean weapon = t.kind() == Proto.ITEM_WEAPON;
		CompoundTag tag = new CompoundTag();
		tag.putInt("id", t.formId());
		tag.putInt("kind", t.kind());
		tag.putInt("class", t.weaponClass());
		tag.putFloat("damage", t.damage());
		tag.putString("name", t.name());
		tag.putString("icon", t.icon());
		tag.putInt("nvcount", t.count());
		tag.putBoolean("link", true);
		tag.putInt("hotkey", hotkey);
		stack.set(DataComponents.CUSTOM_DATA, CustomData.of(wrap(tag)));
		stack.set(DataComponents.ITEM_NAME, Component.literal(t.name()));
		stack.set(DataComponents.ITEM_MODEL, Identifier.parse(model(t)));
		stack.set(DataComponents.MAX_STACK_SIZE, single ? 1 : 99);
		if (weapon && meleeClass(t.weaponClass())) {
			// Swung like Minecraft's own: its damage is New Vegas' (the attack carries it as New
			// Vegas damage), its pace a sword's (one hand), an axe's (two hands) or a fist's.
			double speed = switch (t.weaponClass()) {
				case Proto.CLASS_MELEE_2H -> -3.0;
				case Proto.CLASS_UNARMED -> -1.0;
				default -> -2.4;
			};
			stack.set(DataComponents.ATTRIBUTE_MODIFIERS, ItemAttributeModifiers.builder()
				.add(Attributes.ATTACK_DAMAGE, new AttributeModifier(Item.BASE_ATTACK_DAMAGE_ID, Math.max(1.0F, t.damage()) - 1.0, AttributeModifier.Operation.ADD_VALUE),
					EquipmentSlotGroup.MAINHAND)
				.add(Attributes.ATTACK_SPEED, new AttributeModifier(Item.BASE_ATTACK_SPEED_ID, speed, AttributeModifier.Operation.ADD_VALUE), EquipmentSlotGroup.MAINHAND)
				.build());
		}
		stack.set(DataComponents.LORE, new ItemLore(lore(t)));
		return stack;
	}

	/** The tooltip: where the item lives and its New Vegas numbers (the count, past one stack). */
	public static List<Component> lore(Taken t) {
		boolean weapon = t.kind() == Proto.ITEM_WEAPON;
		List<Component> lore = new ArrayList<>();
		lore.add(Component.literal("Fallout: New Vegas"));
		if (t.count() > 99 || (t.count() > 1 && (weapon || t.kind() == Proto.ITEM_ARMOR))) {
			lore.add(Component.literal("In New Vegas: " + t.count()));
		}
		if (weapon && t.damage() > 0) {
			lore.add(Component.literal("Damage " + Math.round(t.damage())));
		}
		if (t.value() > 0) {
			lore.add(Component.literal("Value " + t.value()));
		}
		if (t.weight() > 0) {
			lore.add(Component.literal("Weight " + String.format("%.1f", t.weight())));
		}
		return lore;
	}

	private static CompoundTag wrap(CompoundTag inner) {
		CompoundTag outer = new CompoundTag();
		outer.put(TAG, inner);
		return outer;
	}

	/** The item model: the New Vegas inventory image when there is one, else a similar vanilla item. */
	public static String model(Taken t) {
		String icon = NvIconNames.model(t.icon());
		if (icon != null) {
			return icon;
		}
		String name = t.name().toLowerCase();
		return switch (t.kind()) {
			case Proto.ITEM_WEAPON -> switch (t.weaponClass()) {
				case Proto.CLASS_PISTOL, Proto.CLASS_AUTOMATIC -> "minecraft:crossbow";
				case Proto.CLASS_RIFLE -> "minecraft:bow";
				case Proto.CLASS_LAUNCHER -> "minecraft:firework_rocket";
				case Proto.CLASS_THROWN -> "minecraft:fire_charge";
				case Proto.CLASS_MELEE_1H -> "minecraft:iron_sword";
				case Proto.CLASS_MELEE_2H -> "minecraft:mace";
				default -> "minecraft:iron_sword";
			};
			case Proto.ITEM_ARMOR -> "minecraft:leather_chestplate";
			case Proto.ITEM_AMMO -> "minecraft:arrow";
			case Proto.ITEM_AID -> name.contains("stimpak") ? "minecraft:splash_potion" : "minecraft:honey_bottle";
			case Proto.ITEM_BOOK -> "minecraft:book";
			case Proto.ITEM_KEY -> "minecraft:tripwire_hook";
			default -> name.contains("cap") ? "minecraft:gold_nugget" : "minecraft:iron_nugget";
		};
	}

	/** The New Vegas tag of a stack, or null for anything else. */
	public static CompoundTag data(ItemStack stack) {
		if (stack.isEmpty()) {
			return null;
		}
		CustomData custom = stack.get(DataComponents.CUSTOM_DATA);
		if (custom == null) {
			return null;
		}
		CompoundTag outer = custom.copyTag();
		return outer.contains(TAG) ? outer.getCompoundOrEmpty(TAG) : null;
	}

	public static boolean isWeapon(ItemStack stack) {
		CompoundTag tag = data(stack);
		return tag != null && tag.getIntOr("kind", -1) == Proto.ITEM_WEAPON;
	}

	private static boolean meleeClass(int weaponClass) {
		return weaponClass == Proto.CLASS_MELEE_1H || weaponClass == Proto.CLASS_MELEE_2H || weaponClass == Proto.CLASS_UNARMED;
	}

	/** A New Vegas melee or unarmed weapon: Minecraft holds and swings it like one of its own. */
	public static boolean isMelee(ItemStack stack) {
		CompoundTag tag = data(stack);
		return tag != null && tag.getIntOr("kind", -1) == Proto.ITEM_WEAPON && meleeClass(tag.getIntOr("class", 0));
	}

	/** A New Vegas weapon New Vegas itself wields, fires and reloads (everything but melee). */
	public static boolean isNvWielded(ItemStack stack) {
		return isWeapon(stack) && !isMelee(stack);
	}

	/**
	 * The New Vegas FormID of a held weapon, 0 for anything else. New Vegas equips every one: ranged
	 * weapons it wields, melee ones it only shows (their real model, in Minecraft's hand).
	 */
	public static int weaponForm(ItemStack stack) {
		CompoundTag tag = data(stack);
		return tag != null && isWeapon(stack) ? tag.getIntOr("id", 0) : 0;
	}

	/** New Vegas damage of a melee stack (applied as is, after Minecraft's cooldown and crit). */
	public static float meleeDamage(ItemStack stack) {
		CompoundTag tag = data(stack);
		return tag != null && isMelee(stack) ? tag.getFloatOr("damage", 0.0F) : 0.0F;
	}
}
