package dev.vegascraft.mixin;

import dev.vegascraft.item.NvItems;
import net.minecraft.world.Container;
import net.minecraft.world.entity.player.Inventory;
import net.minecraft.world.inventory.Slot;
import net.minecraft.world.item.ItemStack;
import org.spongepowered.asm.mixin.Final;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * New Vegas items stay in the player's own inventory: they are copies of New Vegas' inventory
 * (dev.vegascraft.item.NvInventory), so a chest, a furnace or the crafting grid would only hold
 * duplicates.
 */
@Mixin(Slot.class)
abstract class SlotMixin {
	@Shadow
	@Final
	public Container container;

	@Inject(method = "mayPlace", at = @At("HEAD"), cancellable = true)
	private void vegascraft$keepNewVegasItems(ItemStack stack, CallbackInfoReturnable<Boolean> cir) {
		if (!(this.container instanceof Inventory) && NvItems.data(stack) != null) {
			cir.setReturnValue(false);
		}
	}
}
