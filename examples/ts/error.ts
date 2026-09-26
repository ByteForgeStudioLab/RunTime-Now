interface Config {
  retries: number;
}

function connect(cfg: Config): never {
  throw new Error(`ulanib bo'lmadi (${cfg.retries} urinish)`);
}

connect({ retries: 3 } as Config);
