package dev.vegascraft.client.mixin;

import dev.vegascraft.client.PipBoyPage;
import net.minecraft.client.gui.GuiGraphicsExtractor;
import net.minecraft.client.gui.screens.Screen;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/** The Pip-Boy's Minecraft page draws no screen background: the Pip-Boy stays visible around it. */
@Mixin(Screen.class)
abstract class ScreenBackgroundMixin {
	@Inject(method = "extractBlurredBackground", at = @At("HEAD"), cancellable = true)
	private void vegascraft$noBlur(GuiGraphicsExtractor graphics, CallbackInfo ci) {
		if (PipBoyPage.hidesBackground((Screen) (Object) this)) {
			ci.cancel();
		}
	}

	@Inject(method = "extractTransparentBackground", at = @At("HEAD"), cancellable = true)
	private void vegascraft$noDimming(GuiGraphicsExtractor graphics, CallbackInfo ci) {
		if (PipBoyPage.hidesBackground((Screen) (Object) this)) {
			ci.cancel();
		}
	}
}
