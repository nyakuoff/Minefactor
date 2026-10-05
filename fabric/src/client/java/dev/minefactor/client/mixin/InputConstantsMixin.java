package dev.minefactor.client.mixin;

import com.mojang.blaze3d.platform.InputConstants;
import com.mojang.blaze3d.platform.Window;
import dev.minefactor.client.InputBridge;
import dev.minefactor.client.SkyClient;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/** Keyboard state and mouse capture come from Skyrim while linked, not from SDL. */
@Mixin(InputConstants.class)
public abstract class InputConstantsMixin {
	@Inject(method = "isKeyDown", at = @At("HEAD"), cancellable = true)
	private static void minefactor$isKeyDown(int key, CallbackInfoReturnable<Boolean> cir) {
		if (SkyClient.tookOver()) {
			cir.setReturnValue(InputBridge.isKeyDown(key));
		}
	}

	@Inject(method = "grabMouse", at = @At("HEAD"), cancellable = true)
	private static void minefactor$grabMouse(Window window, double xpos, double ypos, CallbackInfo ci) {
		if (SkyClient.tookOver()) {
			ci.cancel();
		}
	}

	@Inject(method = "releaseMouse", at = @At("HEAD"), cancellable = true)
	private static void minefactor$releaseMouse(Window window, double xpos, double ypos, CallbackInfo ci) {
		if (SkyClient.tookOver()) {
			ci.cancel();
		}
	}
}
