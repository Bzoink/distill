/* A nested module: its own import is relative to /modules/, not to the page
   that loaded it. */
import { CHUNK } from "./chunk.js";
import { DEEP } from "./nested/deep.js";

globalThis.__moduleGraph = CHUNK + "/" + DEEP;
