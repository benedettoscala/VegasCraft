package dev.vegascraft.client.icons;

import dev.vegascraft.SkyCraft;
import dev.vegascraft.item.NvIconNames;
import java.awt.image.BufferedImage;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Optional;
import java.util.stream.Stream;
import javax.imageio.ImageIO;
import net.fabricmc.loader.api.FabricLoader;
import net.minecraft.network.chat.Component;
import net.minecraft.server.packs.PackLocationInfo;
import net.minecraft.server.packs.PackSelectionConfig;
import net.minecraft.server.packs.PackType;
import net.minecraft.server.packs.PathPackResources;
import net.minecraft.server.packs.repository.Pack;
import net.minecraft.server.packs.repository.PackSource;
import net.minecraft.server.packs.repository.RepositorySource;

/**
 * New Vegas' inventory images (the Pip-Boy's) as Minecraft item models: read from New Vegas'
 * archives and loose files, tinted in the Pip-Boy's amber with a dark outline, shrunk to 32x32 and
 * written as a resource pack under .minecraft/vegascraft/nvicons, which is always enabled. Made
 * again only when New Vegas' archives change.
 */
public final class NvIcons {
	private static final String PACK_ID = "vegascraft_nvicons";
	private static final int SIZE = 32;
	private static final int AMBER = 0xFFB642, OUTLINE = 0x2A1A05;
	private static final int FORMAT = 5;  // bump to regenerate after changing how icons are made
	private static Path packDir;

	private NvIcons() {
	}

	/** Client start, before the first resource load. */
	public static void init() {
		packDir = FabricLoader.getInstance().getGameDir().resolve("vegascraft").resolve("nvicons");
		long started = System.currentTimeMillis();
		try {
			Path data = findData();
			if (data == null) {
				SkyCraft.LOG.warn("VegasCraft: New Vegas' Data folder not found (set nvDataDir in config/vegascraft.properties): New Vegas items keep vanilla models");
				loadAvailable();
				return;
			}
			String stamp = stamp(data);
			Path stampFile = packDir.resolve("stamp.txt");
			if (!Files.exists(stampFile) || !Files.readString(stampFile).equals(stamp)) {
				int made = generate(data);
				Files.writeString(stampFile, stamp);
				SkyCraft.LOG.info("VegasCraft: made {} New Vegas item icons from {} in {} ms", made, data, System.currentTimeMillis() - started);
			}
			loadAvailable();
		} catch (Throwable t) {
			SkyCraft.LOG.error("VegasCraft: couldn't make the New Vegas item icons", t);
		}
	}

	/** The pack for the client's pack repository (PackRepositoryMixin). */
	public static RepositorySource source() {
		return onLoad -> {
			if (packDir == null || !Files.exists(packDir.resolve("pack.mcmeta"))) {
				return;
			}
			Pack pack = Pack.readMetaAndCreate(new PackLocationInfo(PACK_ID, Component.literal("VegasCraft: New Vegas icons"), PackSource.BUILT_IN, Optional.empty()),
				new PathPackResources.PathResourcesSupplier(packDir), PackType.CLIENT_RESOURCES, new PackSelectionConfig(true, Pack.Position.TOP, false));
			if (pack != null) {
				onLoad.accept(pack);
			}
		};
	}

	private static void loadAvailable() throws IOException {
		Path items = packDir.resolve("assets").resolve(NvIconNames.NAMESPACE).resolve("items");
		List<String> ids = new ArrayList<>();
		if (Files.isDirectory(items)) {
			try (Stream<Path> files = Files.walk(items)) {
				files.filter(p -> p.toString().endsWith(".json")).forEach(p -> {
					String rel = items.relativize(p).toString().replace('\\', '/');
					ids.add(rel.substring(0, rel.length() - ".json".length()));
				});
			}
		}
		NvIconNames.setAvailable(ids);
	}

	/** FalloutNV.exe's Data folder: the running game's (it starts Minecraft), the configured one, or Steam's default. */
	private static Path findData() {
		Optional<Path> running = ProcessHandle.allProcesses()
			.map(p -> p.info().command().orElse(""))
			.filter(c -> c.toLowerCase(Locale.ROOT).endsWith("falloutnv.exe"))
			.map(c -> Path.of(c).getParent().resolve("Data"))
			.findFirst();
		if (running.isPresent() && Files.isDirectory(running.get())) {
			return running.get();
		}
		try {
			Path config = FabricLoader.getInstance().getGameDir().resolve("config").resolve("vegascraft.properties");
			if (Files.exists(config)) {
				var props = new java.util.Properties();
				try (var in = Files.newBufferedReader(config)) {
					props.load(in);
				}
				String dir = props.getProperty("nvDataDir", "").trim();
				if (!dir.isEmpty() && Files.isDirectory(Path.of(dir))) {
					return Path.of(dir);
				}
			}
		} catch (IOException ignored) {
		}
		Path steam = Path.of("C:\\Program Files (x86)\\Steam\\steamapps\\common\\Fallout New Vegas\\Data");
		return Files.isDirectory(steam) ? steam : null;
	}

	private static List<Path> archives(Path data) throws IOException {
		try (Stream<Path> files = Files.list(data)) {
			return files.filter(p -> p.getFileName().toString().toLowerCase(Locale.ROOT).endsWith(".bsa")).sorted().toList();
		}
	}

	private static String stamp(Path data) throws IOException {
		StringBuilder s = new StringBuilder("format " + FORMAT + "\n");
		for (Path bsa : archives(data)) {
			s.append(bsa.getFileName()).append(' ').append(Files.size(bsa)).append(' ').append(Files.getLastModifiedTime(bsa).toMillis()).append('\n');
		}
		Path loose = data.resolve("textures").resolve("interface").resolve("icons");
		if (Files.isDirectory(loose)) {
			try (Stream<Path> files = Files.walk(loose)) {
				s.append("loose ").append(files.filter(Files::isRegularFile).count()).append('\n');
			}
		}
		return s.toString();
	}

	/** Inventory images: interface\icons\pipboyimages\... and the DLCs' (nvdlc01\interface\icons\...), not the small HUD variants. */
	private static boolean wanted(String path) {
		if (!path.endsWith(".dds") || path.contains("pipboyimages_small")) {
			return false;
		}
		if (isMap(path)) {
			return true;
		}
		return path.startsWith("textures\\interface\\icons\\pipboyimages") || path.startsWith("textures\\interface\\icons\\stats\\")
			|| path.startsWith("textures\\interface\\icons\\world map\\") || path.startsWith("textures\\interface\\icons\\local map\\")
			|| path.startsWith("textures\\interface\\icons\\message icons\\") || (path.startsWith("textures\\nvdlc") && path.contains("\\interface\\icons\\"));
	}

	/** The world maps' images: textures\...interface\worldmap\*_1024_no_map.dds (the Pip-Boy draws the markers itself). */
	private static boolean isMap(String path) {
		return path.contains("\\interface\\worldmap\\") && path.endsWith("_1024_no_map.dds");
	}

	/** The id a world map's image has (what its worldspace names, whatever the folder): the file name, letters and digits. */
	public static String mapId(String path) {
		String name = path.toLowerCase(Locale.ROOT).replace('/', '\\');
		name = name.substring(name.lastIndexOf('\\') + 1);
		if (name.endsWith(".dds")) {
			name = name.substring(0, name.length() - 4);
		}
		return name.replaceAll("[^a-z0-9]", "_");
	}

	/** A map in amber on black. */
	private static BufferedImage mapImage(Dds src) {
		BufferedImage out = new BufferedImage(src.width, src.height, BufferedImage.TYPE_INT_ARGB);
		for (int y = 0; y < src.height; y++) {
			for (int x = 0; x < src.width; x++) {
				int p = src.argb[y * src.width + x];
				float lum = Math.max((p >> 16) & 0xFF, Math.max((p >> 8) & 0xFF, p & 0xFF)) / 255.0F;
				lum = Math.min(1.0F, lum * 0.85F + 0.05F);
				out.setRGB(x, y, 0xFF000000 | (Math.round(0xFF * lum) << 16) | (Math.round(0xB6 * lum) << 8) | Math.round(0x42 * lum));
			}
		}
		return out;
	}

	private static int generate(Path data) throws IOException {
		if (Files.exists(packDir)) {
			try (Stream<Path> old = Files.walk(packDir)) {
				old.sorted(java.util.Comparator.reverseOrder()).forEach(p -> p.toFile().delete());
			}
		}
		Files.createDirectories(packDir);
		Files.writeString(packDir.resolve("pack.mcmeta"),
			"{\"pack\":{\"description\":\"New Vegas inventory images (made by VegasCraft)\",\"min_format\":97,\"max_format\":97}}", StandardCharsets.UTF_8);
		// Archives in name order, loose files last: a later source replaces an earlier one's image.
		Map<String, ImageSource> images = new LinkedHashMap<>();
		List<Bsa> open = new ArrayList<>();
		try {
			for (Path path : archives(data)) {
				try {
					Bsa bsa = new Bsa(path);
					open.add(bsa);
					for (Bsa.Entry e : bsa.entries) {
						if (wanted(e.path())) {
							images.put(e.path().substring("textures\\".length()), () -> bsa.bytes(e));
						}
					}
				} catch (IOException ex) {
					SkyCraft.LOG.warn("VegasCraft: skipped {} ({})", path.getFileName(), ex.getMessage());
				}
			}
			Path textures = data.resolve("textures");
			Path loose = textures.resolve("interface").resolve("icons");
			if (Files.isDirectory(loose)) {
				try (Stream<Path> files = Files.walk(loose)) {
					for (Path p : files.filter(Files::isRegularFile).toList()) {
						String rel = ("textures\\" + textures.relativize(p).toString().replace('/', '\\')).toLowerCase(Locale.ROOT);
						if (wanted(rel)) {
							images.put(rel.substring("textures\\".length()), () -> Files.readAllBytes(p));
						}
					}
				}
			}
			int made = 0;
			for (var image : images.entrySet()) {
				if (isMap("textures\\" + image.getKey())) {
					try {
						writeMap(mapId(image.getKey()), mapImage(Dds.decode(image.getValue().bytes())));
					} catch (IOException ex) {
						SkyCraft.LOG.debug("VegasCraft: skipped map {} ({})", image.getKey(), ex.getMessage());
					}
					continue;
				}
				String id = NvIconNames.id(image.getKey());
				if (id == null) {
					continue;
				}
				try {
					write(id, icon(Dds.decode(image.getValue().bytes())));
					made++;
				} catch (IOException ex) {
					SkyCraft.LOG.debug("VegasCraft: skipped icon {} ({})", image.getKey(), ex.getMessage());
				}
			}
			return made;
		} finally {
			for (Bsa bsa : open) {
				bsa.close();
			}
		}
	}

	private interface ImageSource {
		byte[] bytes() throws IOException;
	}

	/** The image fitted (keeping its shape) into SIZE x SIZE, in amber, with a dark outline. */
	private static BufferedImage icon(Dds src) {
		float scale = Math.min((float) SIZE / src.width, (float) SIZE / src.height);
		int w = Math.max(1, Math.round(src.width * scale)), h = Math.max(1, Math.round(src.height * scale));
		int ox = (SIZE - w) / 2, oy = (SIZE - h) / 2;
		float[] alpha = new float[SIZE * SIZE];
		// Box filter: each output pixel averages the source pixels it covers (premultiplied).
		for (int y = 0; y < h; y++) {
			int sy0 = y * src.height / h, sy1 = Math.max(sy0 + 1, (y + 1) * src.height / h);
			for (int x = 0; x < w; x++) {
				int sx0 = x * src.width / w, sx1 = Math.max(sx0 + 1, (x + 1) * src.width / w);
				float sum = 0;
				int n = 0;
				for (int sy = sy0; sy < sy1; sy++) {
					for (int sx = sx0; sx < sx1; sx++) {
						int p = src.argb[sy * src.width + sx];
						int lum = Math.max((p >> 16) & 0xFF, Math.max((p >> 8) & 0xFF, p & 0xFF));
						sum += ((p >>> 24) / 255.0F) * (lum / 255.0F);
						n++;
					}
				}
				alpha[(oy + y) * SIZE + ox + x] = sum / n;
			}
		}
		BufferedImage out = new BufferedImage(SIZE, SIZE, BufferedImage.TYPE_INT_ARGB);
		for (int y = 0; y < SIZE; y++) {
			for (int x = 0; x < SIZE; x++) {
				float a = alpha[y * SIZE + x];
				if (a > 0.08F) {
					int ia = Math.min(255, Math.round(Math.min(1.0F, a * 1.6F) * 255));
					out.setRGB(x, y, (ia << 24) | AMBER);
					continue;
				}
				float near = 0;
				for (int dy = -1; dy <= 1; dy++) {
					for (int dx = -1; dx <= 1; dx++) {
						int nx = x + dx, ny = y + dy;
						if (nx >= 0 && ny >= 0 && nx < SIZE && ny < SIZE) {
							near = Math.max(near, alpha[ny * SIZE + nx]);
						}
					}
				}
				if (near > 0.08F) {
					out.setRGB(x, y, (200 << 24) | OUTLINE);
				}
			}
		}
		return out;
	}

	private static void writeMap(String id, BufferedImage image) throws IOException {
		Path png = packDir.resolve("assets").resolve(NvIconNames.NAMESPACE).resolve("textures").resolve("gui").resolve("map").resolve(id + ".png");
		Files.createDirectories(png.getParent());
		ImageIO.write(image, "png", png.toFile());
	}

	private static void write(String id, BufferedImage image) throws IOException {
		Path assets = packDir.resolve("assets").resolve(NvIconNames.NAMESPACE);
		Path png = assets.resolve("textures").resolve("item").resolve(id + ".png");
		Files.createDirectories(png.getParent());
		ImageIO.write(image, "png", png.toFile());
		Path model = assets.resolve("models").resolve("item").resolve(id + ".json");
		Files.createDirectories(model.getParent());
		Files.writeString(model, "{\"parent\":\"minecraft:item/generated\",\"textures\":{\"layer0\":\"" + NvIconNames.NAMESPACE + ":item/" + id + "\"}}");
		Path item = assets.resolve("items").resolve(id + ".json");
		Files.createDirectories(item.getParent());
		Files.writeString(item, "{\"model\":{\"type\":\"minecraft:model\",\"model\":\"" + NvIconNames.NAMESPACE + ":item/" + id + "\"}}");
	}
}
