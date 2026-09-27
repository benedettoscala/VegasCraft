package dev.vegascraft.client.mixin;

import dev.vegascraft.client.PipBoyPage;
import net.minecraft.client.gui.screens.inventory.CraftingScreen;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.ModifyArg;

/**
 * CraftingScreen draws its panel at the window's vertical centre ((height - imageHeight) / 2)
 * rather than at topPos: on the Pip-Boy's Minecraft page, moved onto the Pip-Boy's screen, it
 * follows topPos like its slots do.
 */
@Mixin(CraftingScreen.class)
abstract class CraftingScreenMixin {
	@ModifyArg(method = "extractBackground", at = @At(value = "INVOKE",
		target = "Lnet/minecraft/client/gui/GuiGraphicsExtractor;blit(Lcom/mojang/renderpearl/api/pipeline/RenderPipeline;Lnet/minecraft/resources/Identifier;IIFFIIII)V"), index = 3)
	private int vegascraft$panelAtTopPos(int y) {
		return PipBoyPage.active() ? ((AbstractContainerScreenAccessor) this).vegascraft$topPos() : y;
	}
}
