package dev.vegascraft.client.mixin;

import dev.vegascraft.client.NvAvatarArms;
import dev.vegascraft.client.NvHand;
import net.minecraft.client.model.HumanoidModel;
import net.minecraft.client.model.geom.ModelPart;
import net.minecraft.client.renderer.entity.state.AvatarRenderState;
import net.minecraft.client.renderer.entity.state.HumanoidRenderState;
import org.spongepowered.asm.mixin.Final;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/** The captured avatar's arms (and its armour's) on New Vegas' third-person skeleton (NvAvatarArms). */
@Mixin(HumanoidModel.class)
public abstract class HumanoidModelMixin {
	@Shadow
	@Final
	public ModelPart rightArm;
	@Shadow
	@Final
	public ModelPart leftArm;

	@Inject(method = "setupAnim(Lnet/minecraft/client/renderer/entity/state/HumanoidRenderState;)V", at = @At("TAIL"))
	private void vegascraft$nvArms(HumanoidRenderState state, CallbackInfo ci) {
		if (NvHand.capturing && state instanceof AvatarRenderState) {
			NvAvatarArms.apply(this.rightArm, this.leftArm, state.bodyRot);
		}
	}
}
