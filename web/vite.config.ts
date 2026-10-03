import { defineConfig } from 'vite';
import { svelte } from '@sveltejs/vite-plugin-svelte';

// `npm run dev` proxies the device API to DEVICE (default: the host build of
// the server, host/airprint_host on port 8631).
const device = process.env.DEVICE || 'http://localhost:8631';
const proxy = Object.fromEntries(
  ['/api', '/events', '/scan', '/ipp', '/eSCL', '/icon.png'].map((p) => [p, { target: device, changeOrigin: true }]),
);

export default defineConfig({
  plugins: [svelte()],
  build: {
    target: ['es2020', 'safari14'],
    sourcemap: false,
    chunkSizeWarningLimit: 2500,
    modulePreload: { polyfill: false },
  },
  server: { proxy },
});
