package dev.vegascraft.client.mixin;

import com.llamalad7.mixinextras.injector.wrapoperation.Operation;
import com.llamalad7.mixinextras.injector.wrapoperation.WrapOperation;
import com.llamalad7.mixinextras.sugar.Local;
import com.mojang.blaze3d.vertex.PoseStack;
import dev.vegascraft.client.NvHand;
import dev.vegascraft.client.SkyClient;
import dev.vegascraft.item.NvItems;
import net.minecraft.client.renderer.SubmitNodeCollector;
import net.minecraft.client.renderer.entity.layers.ItemInHandLayer;
import net.minecraft.client.renderer.item.ItemStackRenderState;
import net.minecraft.world.item.ItemStack;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;

/**
 * A held New Vegas weapon isn't drawn as its Minecraft stand-in: New Vegas draws the real model in
 * the avatar's hand, at the pose captured here (NvHand).
 */
@Mixin(ItemInHandLayer.class)
public abstract class ItemInHandLayerMixin {
	@WrapOperation(method = "submitArmWithItem", at = @At(value = "INVOKE",
		target = "Lnet/minecraft/client/renderer/item/ItemStackRenderState;submit(Lcom/mojang/blaze3d/vertex/PoseStack;Lnet/minecraft/client/renderer/SubmitNodeCollector;III)V"))
	private void vegascraft$nvWeaponInHand(ItemStackRenderState item, PoseStack pose, SubmitNodeCollector collector, int light, int overlay, int outline,
		Operation<Void> original, @Local(argsOnly = true) ItemStack stack) {
		if (!SkyClient.linked() || !NvItems.isWeapon(stack)) {
			original.call(item, pose, collector, light, overlay, outline);
			return;
		}
		if (NvHand.capturing) {
			NvHand.record(pose.last().pose());
		}
	}
}
