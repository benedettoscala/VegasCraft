package dev.vegascraft.client.mixin;

import com.google.common.collect.ImmutableSet;
import dev.vegascraft.client.icons.NvIcons;
import java.util.Set;
import net.minecraft.client.resources.ClientPackSource;
import net.minecraft.server.packs.repository.PackRepository;
import net.minecraft.server.packs.repository.RepositorySource;
import org.spongepowered.asm.mixin.Final;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Mutable;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/** The client's resource packs include the New Vegas item icons (NvIcons), always enabled. */
@Mixin(PackRepository.class)
abstract class PackRepositoryMixin {
	@Shadow
	@Final
	@Mutable
	private Set<RepositorySource> sources;

	@Inject(method = "<init>", at = @At("RETURN"))
	private void vegascraft$addNewVegasIcons(RepositorySource[] given, CallbackInfo ci) {
		if (this.sources.stream().anyMatch(s -> s instanceof ClientPackSource)) {
			this.sources = ImmutableSet.<RepositorySource>builder().addAll(this.sources).add(NvIcons.source()).build();
		}
	}
}
