# The relay's image, for Fly (fly.toml): Node and the relay, nothing else.
FROM node:24-alpine
WORKDIR /relay
COPY relay.mjs main.mjs ./
USER node
ENV PORT=8080
EXPOSE 8080
CMD ["node", "main.mjs"]
