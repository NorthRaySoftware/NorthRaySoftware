// Static site: plain HTML/CSS output, hostable on any static host.
import { defineConfig } from 'astro/config';

export default defineConfig({
  output: 'static',
  // 'assets', not Astro's default '_astro': GitHub Pages hides folders that
  // start with an underscore unless the site has a .nojekyll file.
  build: { assets: 'assets' },
  // Set this once a real domain exists. Until then the site is local only.
  // site: 'https://example.invalid',
  vite: {
    server: { fs: { allow: ['..'] } },
  },
});
