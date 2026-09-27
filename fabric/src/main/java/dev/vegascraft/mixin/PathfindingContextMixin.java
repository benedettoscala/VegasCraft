package dev.vegascraft.mixin;

import dev.vegascraft.world.NvLand;
import dev.vegascraft.world.SkyCollision;
import net.minecraft.core.BlockPos;
import net.minecraft.world.level.pathfinder.PathType;
import net.minecraft.world.level.pathfinder.PathfindingContext;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * Mobs find their way over New Vegas' ground: an air cell New Vegas fills (the ground reaching
 * its top, a wall, a rock) is blocked for pathfinding, so the cell above it is walkable. Without
 * this Minecraft sees only air under New Vegas' ground and its mobs never set off.
 */
@Mixin(PathfindingContext.class)
public abstract class PathfindingContextMixin {
	@Inject(method = "getPathTypeFromState(III)Lnet/minecraft/world/level/pathfinder/PathType;", at = @At("RETURN"), cancellable = true)
	private void skycraft$newVegasGround(int x, int y, int z, CallbackInfoReturnable<PathType> cir) {
		if (cir.getReturnValue() != PathType.OPEN || !SkyCollision.active()) {
			return;
		}
		BlockPos pos = new BlockPos(x, y, z);
		boolean solid = SkyCollision.supportsFromBelow(pos.above()) || SkyCollision.solidFraction(pos) >= 0.5F;
		if (!solid) {
			double ground = NvLand.height(x, z);
			solid = !Double.isNaN(ground) && y + 1 <= ground + 0.25;  // under the land's surface
		}
		if (solid) {
			cir.setReturnValue(PathType.BLOCKED);
		}
	}
}
