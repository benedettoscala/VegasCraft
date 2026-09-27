package dev.vegascraft.client.mixin;

import dev.vegascraft.client.NvArmPose;
import dev.vegascraft.client.SkyClient;
import net.minecraft.client.DeltaTracker;
import net.minecraft.client.gui.GuiGraphicsExtractor;
import net.minecraft.client.gui.Hud;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/**
 * While New Vegas' Pip-Boy is up, Minecraft draws only its arms and Pip-Boy: no HUD over it. Only
 * the HUD: a screen (the Pip-Boy's Minecraft page) still draws.
 */
@Mixin(Hud.class)
public abstract class GuiMixin {
	@Inject(method = "extractRenderState", at = @At("HEAD"), cancellable = true)
	private void vegascraft$noHudOverPipBoy(GuiGraphicsExtractor graphics, DeltaTracker delta, CallbackInfo ci) {
		if (SkyClient.linked() && NvArmPose.pipBoy()) {
			ci.cancel();
		}
	}
}
