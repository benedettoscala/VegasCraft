package dev.vegascraft.client.mixin;

import dev.vegascraft.client.SkyDigClient;
import dev.vegascraft.item.NvItems;
import net.minecraft.client.Minecraft;
import net.minecraft.world.InteractionHand;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/** Attacking Skyrim's geometry digs into it (SkyDigClient); a New Vegas weapon is New Vegas' to fire. */
@Mixin(Minecraft.class)
public abstract class MinecraftDigMixin {
	@Inject(method = "startAttack", at = @At("HEAD"), cancellable = true)
	private void skycraft$digStart(CallbackInfoReturnable<Boolean> cir) {
		Minecraft minecraft = (Minecraft) (Object) this;
		if (minecraft.player != null && NvItems.isNvWielded(minecraft.player.getMainHandItem())) {
			cir.setReturnValue(false); // New Vegas fires it (the button goes to New Vegas)
			return;
		}
		if (SkyDigClient.attack(minecraft)) {
			// A swing, not a miss: no miss cooldown before mining the block that appears.
			var held = minecraft.player.getItemInHand(InteractionHand.MAIN_HAND);
			minecraft.player.swing(InteractionHand.MAIN_HAND, held.getAttackAnimation(), false);
			cir.setReturnValue(true);
		}
	}

	@Inject(method = "continueAttack", at = @At("HEAD"), cancellable = true)
	private void skycraft$digHold(boolean down, CallbackInfo ci) {
		Minecraft minecraft = (Minecraft) (Object) this;
		if (minecraft.player != null && NvItems.isNvWielded(minecraft.player.getMainHandItem())) {
			ci.cancel();
			return;
		}
		if (down) {
			SkyDigClient.attack((Minecraft) (Object) this);
		}
	}
}
