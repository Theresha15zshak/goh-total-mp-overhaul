// Skill rating: Weng-Lin (the OpenSkill family), Bradley-Terry with full pairing, for two teams.
// Each player has mu (skill) and sigma (how unsure we are of it); a team is the sum of its players. The win
// probability comes from the teams' mu, and each player moves in proportion to their own share of the
// team's uncertainty: new players move fast, settled ones slowly. Only who won counts.

export const MU = 25, SIGMA = 25 / 3;
const BETA = 25 / 6, KAPPA = 1e-4;

// a, b: arrays of {mu, sigma}; aWon: whether team a won. Returns the new [a, b].
export function rate(a, b, aWon) {
  const team = t => ({ mu: t.reduce((s, p) => s + p.mu, 0), v: t.reduce((s, p) => s + p.sigma * p.sigma, 0) });
  const A = team(a), B = team(b);
  const c = Math.sqrt(A.v + B.v + 2 * BETA * BETA);
  const pA = 1 / (1 + Math.exp((B.mu - A.mu) / c)), pB = 1 - pA;
  const move = (t, T, score, p) => {
    const omega = T.v / c * (score - p);
    const delta = Math.sqrt(T.v) / c * T.v / (c * c) * pA * pB;
    return t.map(x => {
      const share = x.sigma * x.sigma / T.v;
      return { mu: x.mu + share * omega, sigma: x.sigma * Math.sqrt(Math.max(1 - share * delta, KAPPA)) };
    });
  };
  return [move(a, A, aWon ? 1 : 0, pA), move(b, B, aWon ? 0 : 1, pB)];
}

// Before each match the uncertainty grows a little (TAU), and more after IDLE_S without games: a settled player
// still moves, and one back from a break finds their level again quickly. mu itself never decays.
const TAU = 25 / 300, IDLE_S = 30 * 86400, IDLE_STEP = 0.8;
export function drift(t, now) {
  let v = t.sigma * t.sigma + TAU * TAU;
  if (t.last_at && now - t.last_at > IDLE_S) v += IDLE_STEP * IDLE_STEP * (now - t.last_at - IDLE_S) / IDLE_S;
  return { ...t, sigma: Math.min(Math.sqrt(v), SIGMA) };
}

// the same players again within a day: the 4th match on moves ratings less
export const repeatFactor = earlier => [1, 1, 1, 0.8, 0.7, 0.6][earlier] ?? 0.5;

// what players see: 1500 for a new player, about 40 per point of mu
export const mmr = mu => Math.round(1500 + 40 * (mu - MU));
export const CALIBRATION_GAMES = 10;
