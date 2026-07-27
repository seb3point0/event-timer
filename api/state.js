import Redis from 'ioredis';

// Reuse connection across warm function instances.
let client;
function getClient() {
  if (!client) {
    client = new Redis(process.env.REDIS_URL, {
      maxRetriesPerRequest: 2,
      connectTimeout: 5000,
      tls: process.env.REDIS_URL?.startsWith('rediss://') ? {} : undefined,
    });
  }
  return client;
}

const IDLE = { status: 'idle', endTime: null, reminderMs: 0 };

export default async function handler(req, res) {
  res.setHeader('Cache-Control', 'no-store');

  try {
    const redis = getClient();

    if (req.method === 'GET') {
      const raw = await redis.get('timer');
      const state = raw ? JSON.parse(raw) : IDLE;
      return res.json(state);
    }

    if (req.method === 'POST') {
      const { action, presentationSeconds, reminderSeconds } = req.body;

      if (action === 'start') {
        const state = {
          status: 'running',
          endTime: Date.now() + Math.round(presentationSeconds * 1000),
          reminderMs: Math.round(reminderSeconds * 1000),
        };
        await redis.set('timer', JSON.stringify(state));
        return res.json(state);
      }

      if (action === 'reset') {
        await redis.set('timer', JSON.stringify(IDLE));
        return res.json(IDLE);
      }

      return res.status(400).json({ error: 'unknown action' });
    }

    res.status(405).end();
  } catch (err) {
    console.error('state handler error:', err);
    res.status(500).json({ error: err.message });
  }
}
