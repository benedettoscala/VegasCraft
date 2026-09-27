package dev.vegascraft.item;

import java.util.Collection;
import java.util.Locale;
import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;

/**
 * Item models for New Vegas inventory images. The client turns the images in New Vegas' archives
 * into a resource pack ({@code dev.vegascraft.client.NvIcons}) and lists them here; a New Vegas
 * item then uses its own image as its model. Without one (a dedicated server, an image the pack
 * doesn't have) the item keeps a similar vanilla model.
 */
public final class NvIconNames {
	public static final String NAMESPACE = "vegascraft";
	private static final Set<String> AVAILABLE = ConcurrentHashMap.newKeySet();

	private NvIconNames() {
	}

	/**
	 * The resource path for a New Vegas image path relative to Data/Textures, e.g.
	 * {@code interface\icons\pipboyimages\weapons\weapons_10mm_pistol.dds} becomes
	 * {@code nvicon/pipboyimages/weapons/weapons_10mm_pistol}; null if it isn't an image path.
	 */
	public static String id(String imagePath) {
		if (imagePath == null) {
			return null;
		}
		String p = imagePath.toLowerCase(Locale.ROOT).replace('\\', '/');
		if (p.startsWith("textures/")) {
			p = p.substring("textures/".length());
		}
		if (p.startsWith("interface/icons/")) {
			p = p.substring("interface/icons/".length());
		}
		if (!p.endsWith(".dds") || p.length() <= 4) {
			return null;
		}
		p = p.substring(0, p.length() - 4);
		StringBuilder out = new StringBuilder("nvicon/");
		for (char c : p.toCharArray()) {
			out.append((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '/' || c == '_' || c == '-' || c == '.' ? c : '_');
		}
		return out.toString();
	}

	/** The item model id for an image, or null when the icon pack doesn't have it. */
	public static String model(String imagePath) {
		String id = id(imagePath);
		return id != null && AVAILABLE.contains(id) ? NAMESPACE + ":" + id : null;
	}

	public static void setAvailable(Collection<String> ids) {
		AVAILABLE.clear();
		AVAILABLE.addAll(ids);
	}
}
