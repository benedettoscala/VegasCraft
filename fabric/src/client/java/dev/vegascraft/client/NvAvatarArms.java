package dev.vegascraft.client;

import dev.vegascraft.link.Proto;
import dev.vegascraft.link.SkyLink;
import net.minecraft.client.model.geom.ModelPart;
import org.joml.Vector3f;

/**
 * Third person: the avatar's arms follow New Vegas' animated third-person skeleton (ArmPose,
 * ARMS_THIRD_PERSON), so they aim, fire and reload the way New Vegas animates the wielded weapon.
 * Each arm points from the shoulder bone to the hand bone, in the world, turned into the model's
 * frame with the avatar's body yaw.
 */
public final class NvAvatarArms {
	private static final float[] pose = new float[1 + 2 * Proto.AP_ARM_FLOATS];

	private NvAvatarArms() {
	}

	/** Called after HumanoidModel.setupAnim for the captured avatar. */
	public static void apply(ModelPart rightArm, ModelPart leftArm, float bodyRot) {
		if (SkyLink.readArmPose(pose) != Proto.ARMS_THIRD_PERSON) {
			return;
		}
		aim(rightArm, 1, bodyRot);
		aim(leftArm, 1 + Proto.AP_ARM_FLOATS, bodyRot);
	}

	private static void aim(ModelPart arm, int o, float bodyRot) {
		Vector3f d = new Vector3f(pose[o + 3] - pose[o], pose[o + 4] - pose[o + 1], pose[o + 5] - pose[o + 2]);
		if (d.lengthSquared() < 1.0E-8F) {
			return;
		}
		d.normalize();
		// World -> model: the renderer turns the model by 180 - bodyRot about Y and flips x and y.
		float a = (float) Math.toRadians(180.0F - bodyRot);
		float c = (float) Math.cos(-a), s = (float) Math.sin(-a);
		float x = c * d.x + s * d.z, z = -s * d.x + c * d.z;
		float mx = -x, my = -d.y, mz = z;
		// The arm hangs along +Y; ZYX rotation with yRot = 0: (-sin z cos x, cos z cos x, sin x).
		arm.xRot = (float) Math.asin(Math.max(-1.0F, Math.min(1.0F, mz)));
		arm.yRot = 0.0F;
		arm.zRot = (float) Math.atan2(-mx, my);
	}
}
