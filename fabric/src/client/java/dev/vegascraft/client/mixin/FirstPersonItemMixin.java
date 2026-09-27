package dev.vegascraft.client.mixin;

import com.llamalad7.mixinextras.injector.wrapoperation.Operation;
import com.llamalad7.mixinextras.injector.wrapoperation.WrapOperation;
import com.llamalad7.mixinextras.sugar.Local;
import com.mojang.blaze3d.vertex.PoseStack;
import dev.vegascraft.client.NvHand;
import dev.vegascraft.client.SkyClient;
import dev.vegascraft.item.NvItems;
import net.minecraft.client.Minecraft;
import net.minecraft.client.renderer.FirstPersonHandsAndItemsRenderer;
import net.minecraft.client.renderer.SubmitNodeCollector;
import net.minecraft.client.renderer.item.ItemStackRenderState;
import net.minecraft.world.InteractionHand;
import net.minecraft.world.item.ItemStack;
import org.joml.Matrix4f;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;

/**
 * First person, a New Vegas melee weapon in the main hand: Minecraft holds and swings it as usual
 * but doesn't draw its stand-in; New Vegas draws the real model at the pose captured here.
 */
@Mixin(FirstPersonHandsAndItemsRenderer.class)
public abstract class FirstPersonItemMixin {
	@WrapOperation(method = "submitArmWithItem", at = @At(value = "INVOKE",
		target = "Lnet/minecraft/client/renderer/item/ItemStackRenderState;submit(Lcom/mojang/blaze3d/vertex/PoseStack;Lnet/minecraft/client/renderer/SubmitNodeCollector;III)V"))
	private void vegascraft$nvMeleeModel(ItemStackRenderState item, PoseStack pose, SubmitNodeCollector collector, int light, int overlay, int outline,
		Operation<Void> original, @Local(argsOnly = true) ItemStack stack, @Local(argsOnly = true) InteractionHand hand) {
		if (!SkyClient.linked() || !NvItems.isMelee(stack)) {
			original.call(item, pose, collector, light, overlay, outline);
			return;
		}
		if (hand == InteractionHand.MAIN_HAND) {
			// renderItemInHand starts the stack with the inverse view rotation (the model-view matrix
			// applies it again): with it applied here the pose is in view space.
			var camera = Minecraft.getInstance().gameRenderer.gameRenderState().levelRenderState.cameraRenderState;
			NvHand.recordFirstPerson(new Matrix4f(camera.viewRotationMatrix).mul(pose.last().pose()));
		}
	}
}
