package dev.vegascraft.client;

import com.mojang.blaze3d.vertex.PoseStack;
import com.mojang.math.Axis;
import dev.vegascraft.link.Proto;
import dev.vegascraft.link.SkyLink;
import java.nio.file.Files;
import java.nio.file.Path;
import net.minecraft.client.Minecraft;
import net.minecraft.world.entity.HumanoidArm;
import org.joml.Matrix4f;
import org.joml.Vector3f;

/**
 * Minecraft's arms on New Vegas' first-person skeleton (ArmPose): New Vegas animates the bones for
 * every weapon (grip, firing, reloading), each arm's box runs along the forearm and ends at the
 * wrist, turned with the hand bone. The bones are in New Vegas' camera space; their x/y are
 * rescaled from New Vegas' first-person FOV to the one Minecraft draws hands with, so they land on
 * the same pixels as the weapon New Vegas draws.
 *
 * config/vegascraft_arms.txt (lines "key value") overrides the tuning and is re-read when it
 * changes: scale (blocks per New Vegas unit), handFov (degrees), axis (hand bone axis the arm's
 * width follows, 0..2, negative flips), roll (degrees about the forearm), fist (blocks past the
 * wrist), showArms (1 draws the arms with a held New Vegas weapon; default 0, hidden), back (New Vegas units the arms sit deeper, behind the weapon), nvTan (overrides New Vegas' tan(vertical FOV / 2)).
 */
public final class NvArmPose {
	private static final float[] pose = new float[1 + 2 * Proto.AP_ARM_FLOATS + 9];
	private static volatile boolean pipBoy;
	private static float scale = 0.08F, handFov = 70.0F, axis = 1, roll = 0.0F, fist = 0.2F, nvTan = 0.0F, back = 4.0F;
	private static boolean showArms;
	private static long checkedAt, modified = -1;

	private NvArmPose() {
	}

	/** Whether Minecraft's arms are drawn while a New Vegas weapon is held (config showArms 1; off by default). */
	public static boolean showArms() {
		reload();
		return showArms;
	}

	/** Reads this frame's bones; false when New Vegas sends none (Minecraft then draws no arm). */
	public static boolean update() {
		reload();
		int space = SkyLink.readArmPose(pose);
		pipBoy = space == Proto.ARMS_PIPBOY && pose[0] > 0.01F;
		return (space == Proto.ARMS_FIRST_PERSON || space == Proto.ARMS_PIPBOY) && pose[0] > 0.01F;
	}

	/** New Vegas' Pip-Boy is up (as of the last update): Minecraft draws its own around the screen. */
	public static boolean pipBoy() {
		return pipBoy;
	}

	/**
	 * The Pip-Boy model's transform (models/item/pipboy.json, drawn with no display transform): its
	 * screen window (x 3..13, y 4..12 at z 8.5, model units) onto New Vegas' Pip-Boy screen.
	 */
	public static void applyPipBoy(PoseStack stack) {
		int o = 1 + 2 * Proto.AP_ARM_FLOATS;
		float fit = (float) Math.tan(Math.toRadians(handFov) / 2.0) / (nvTan > 0 ? nvTan : pose[0]);
		Vector3f centre = new Vector3f(pose[o] * fit, pose[o + 1] * fit, pose[o + 2]).mul(scale);
		Vector3f w = new Vector3f(pose[o + 3] * fit, pose[o + 4] * fit, pose[o + 5]).mul(scale);
		Vector3f h = new Vector3f(pose[o + 6] * fit, pose[o + 7] * fit, pose[o + 8]).mul(scale);
		float halfW = w.length(), halfH = h.length();
		if (halfW < 1.0E-5F || halfH < 1.0E-5F) {
			return;
		}
		Vector3f x = new Vector3f(w).div(halfW);
		Vector3f y = new Vector3f(h).sub(new Vector3f(x).mul(h.dot(x))).normalize();
		Vector3f z = new Vector3f(x).cross(y);  // out of the screen, towards the viewer
		if (z.z < 0) {
			z.negate();
			y.negate();
		}
		// Model units: the window is 10 wide and 8 tall around (8, 8, 8.5); the item renderer
		// draws a model unit as 1/16 of a block from (-0.5, -0.5, -0.5).
		float sx = halfW / (5.0F / 16.0F), sy = halfH / (4.0F / 16.0F), sz = (sx + sy) / 2.0F;
		Matrix4f m = new Matrix4f(
			x.x * sx, x.y * sx, x.z * sx, 0,
			y.x * sy, y.y * sy, y.z * sy, 0,
			z.x * sz, z.y * sz, z.z * sz, 0,
			centre.x, centre.y, centre.z, 1);
		stack.mulPose(m);
		stack.translate(0.0F, 0.0F, -0.5F / 16.0F);  // window plane (z 8.5) onto the screen
	}

	/** Applies the arm's transform: renderPlayerHand then draws the arm model in place. */
	public static void apply(PoseStack stack, HumanoidArm arm) {
		int o = 1 + (arm == HumanoidArm.RIGHT ? 0 : Proto.AP_ARM_FLOATS);
		float fit = (float) Math.tan(Math.toRadians(handFov) / 2.0) / (nvTan > 0 ? nvTan : pose[0]);
		Vector3f elbow = pushBack(pose[o] * fit, pose[o + 1] * fit, pose[o + 2]).mul(scale);
		Vector3f hand = pushBack(pose[o + 3] * fit, pose[o + 4] * fit, pose[o + 5]).mul(scale);
		int column = Math.min(2, Math.abs((int) axis));
		Vector3f side = new Vector3f(pose[o + 6 + column], pose[o + 9 + column], pose[o + 12 + column]);
		if (axis < 0) {
			side.negate();
		}
		// The arm model: shoulder at y = 0, hand at y = 12/16, box centred 6/16 off the pivot.
		Vector3f y = new Vector3f(hand).sub(elbow).normalize();
		Vector3f x = new Vector3f(side).sub(new Vector3f(y).mul(side.dot(y)));
		if (x.lengthSquared() < 1.0E-6F) {
			x.set(1, 0, 0).sub(new Vector3f(y).mul(y.x));
		}
		x.normalize();
		Vector3f z = new Vector3f(x).cross(y);
		Vector3f wrist = new Vector3f(hand).add(new Vector3f(y).mul(fist));
		float centre = arm == HumanoidArm.RIGHT ? -6.0F / 16.0F : 6.0F / 16.0F;
		Matrix4f m = new Matrix4f(
			x.x, x.y, x.z, 0,
			y.x, y.y, y.z, 0,
			z.x, z.y, z.z, 0,
			wrist.x, wrist.y, wrist.z, 1);
		stack.mulPose(m);
		stack.rotateDegrees(Axis.YP, roll);
		stack.translate(-centre, -12.0F / 16.0F, 0.0F);
	}

	/** A bone moved `back` New Vegas units away from the camera along its own ray (same pixels, deeper): the weapon then hides the fingers behind it. */
	private static Vector3f pushBack(float x, float y, float z) {
		if (z > -1.0F) {
			return new Vector3f(x, y, z);
		}
		float k = (z - back) / z;
		return new Vector3f(x * k, y * k, z - back);
	}

	private static void reload() {
		long now = System.currentTimeMillis();
		if (now - checkedAt < 500) {
			return;
		}
		checkedAt = now;
		Path file = Minecraft.getInstance().gameDirectory.toPath().resolve("config").resolve("vegascraft_arms.txt");
		try {
			long stamp = Files.exists(file) ? Files.getLastModifiedTime(file).toMillis() : 0;
			if (stamp == modified) {
				return;
			}
			modified = stamp;
			scale = 0.08F;
			handFov = 70.0F;
			axis = 1;
			roll = 0.0F;
			fist = 0.2F;
			back = 4.0F;
			showArms = false;
			nvTan = 0.0F;
			if (stamp == 0) {
				return;
			}
			for (String line : Files.readAllLines(file)) {
				String[] w = line.trim().split("\\s+");
				if (w.length != 2) {
					continue;
				}
				float v = Float.parseFloat(w[1]);
				switch (w[0]) {
					case "scale" -> scale = v;
					case "handFov" -> handFov = v;
					case "axis" -> axis = v;
					case "roll" -> roll = v;
					case "fist" -> fist = v;
					case "back" -> back = v;
					case "showArms" -> showArms = v != 0;
					case "nvTan" -> nvTan = v;
					default -> {
					}
				}
			}
		} catch (Exception e) {
			dev.vegascraft.SkyCraft.LOG.warn("VegasCraft: bad vegascraft_arms.txt: {}", e.toString());
		}
	}
}
