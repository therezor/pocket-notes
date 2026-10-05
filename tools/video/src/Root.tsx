import { Composition } from "remotion";
import { Promo, TOTAL_FRAMES } from "./Promo";
import { FPS } from "./beats";

export const RemotionRoot = () => (
  <Composition id="Promo" component={Promo} durationInFrames={TOTAL_FRAMES}
    fps={FPS} width={1920} height={1080} />
);
