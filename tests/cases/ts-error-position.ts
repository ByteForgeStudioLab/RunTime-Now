interface Config {
  retries: number;
}

function connect(cfg: Config): never {
  throw new Error(`failed after ${cfg.retries} tries`);
}

connect({ retries: 3 } as Config);
