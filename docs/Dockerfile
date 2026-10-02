# The site on Fly.io (see fly.toml), built beforehand by `npm run build`: its
# pages need the rest of the repo (the grammar, the code they show, the demos),
# so the image only takes what the build made. The server needs nothing from
# node_modules: the build bundles it all.
FROM node:24-alpine

WORKDIR /site
ENV NODE_ENV=production PORT=3000
COPY package.json headers.js server.js ./
COPY build ./build

USER node
EXPOSE 3000
CMD ["node", "server.js"]
