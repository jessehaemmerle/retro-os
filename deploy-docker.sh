#!/bin/bash
set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

echo -e "${BLUE}🚀 RetroOS Docker Deployment Script${NC}"
echo -e "${BLUE}====================================${NC}"

# Function to display usage
show_usage() {
    echo -e "${YELLOW}Usage: $0 [OPTIONS]${NC}"
    echo -e "${YELLOW}Options:${NC}"
    echo -e "  -e, --env ENV        Set environment (dev|prod) [default: prod]"
    echo -e "  -d, --detach         Run in detached mode"
    echo -e "  -b, --build          Force rebuild of images"
    echo -e "  -c, --clean          Clean up existing containers and volumes"
    echo -e "  -h, --help           Show this help message"
    echo -e ""
    echo -e "${YELLOW}Examples:${NC}"
    echo -e "  $0                   # Deploy in production mode"
    echo -e "  $0 -e dev            # Deploy in development mode"
    echo -e "  $0 -b -d             # Force rebuild and run detached"
    echo -e "  $0 -c                # Clean up and deploy fresh"
}

# Default values
ENVIRONMENT="prod"
DETACH_MODE=""
FORCE_BUILD=""
CLEAN_UP=""

# Parse command line arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        -e|--env)
            ENVIRONMENT="$2"
            shift 2
            ;;
        -d|--detach)
            DETACH_MODE="-d"
            shift
            ;;
        -b|--build)
            FORCE_BUILD="--build"
            shift
            ;;
        -c|--clean)
            CLEAN_UP="true"
            shift
            ;;
        -h|--help)
            show_usage
            exit 0
            ;;
        *)
            echo -e "${RED}❌ Unknown option: $1${NC}"
            show_usage
            exit 1
            ;;
    esac
done

# Validate environment
if [[ "$ENVIRONMENT" != "dev" && "$ENVIRONMENT" != "prod" ]]; then
    echo -e "${RED}❌ Invalid environment: $ENVIRONMENT${NC}"
    echo -e "${YELLOW}Valid environments: dev, prod${NC}"
    exit 1
fi

# Set compose file based on environment
if [[ "$ENVIRONMENT" == "prod" ]]; then
    COMPOSE_FILE="docker-compose.prod.yml"
else
    COMPOSE_FILE="docker-compose.yml"
fi

echo -e "${GREEN}🔧 Configuration:${NC}"
echo -e "  Environment: $ENVIRONMENT"
echo -e "  Compose File: $COMPOSE_FILE"
echo -e "  Detached Mode: ${DETACH_MODE:-false}"
echo -e "  Force Build: ${FORCE_BUILD:-false}"
echo -e "  Clean Up: ${CLEAN_UP:-false}"
echo -e ""

# Check if Docker is installed
if ! command -v docker &> /dev/null; then
    echo -e "${RED}❌ Docker is not installed${NC}"
    echo -e "${YELLOW}Please install Docker first: https://docs.docker.com/get-docker/${NC}"
    exit 1
fi

# Check if Docker Compose is available
if ! docker compose version &> /dev/null; then
    echo -e "${RED}❌ Docker Compose is not available${NC}"
    echo -e "${YELLOW}Please install Docker Compose: https://docs.docker.com/compose/install/${NC}"
    exit 1
fi

# Clean up if requested
if [[ "$CLEAN_UP" == "true" ]]; then
    echo -e "${YELLOW}🧹 Cleaning up existing containers and volumes...${NC}"
    docker compose -f "$COMPOSE_FILE" down -v --remove-orphans || true
    docker system prune -f || true
    echo -e "${GREEN}✅ Cleanup completed${NC}"
fi

# Build and start services
echo -e "${YELLOW}🏗️  Building and starting services...${NC}"
docker compose -f "$COMPOSE_FILE" up $DETACH_MODE $FORCE_BUILD

# If running in detached mode, show status
if [[ -n "$DETACH_MODE" ]]; then
    echo -e "${GREEN}🎉 Services started successfully!${NC}"
    echo -e "${YELLOW}📋 Service Status:${NC}"
    docker compose -f "$COMPOSE_FILE" ps
    echo -e ""
    echo -e "${GREEN}🌐 Access URLs:${NC}"
    echo -e "  - Frontend: http://localhost"
    echo -e "  - Backend API: http://localhost/api"
    echo -e "  - Health Check: http://localhost/health"
    echo -e ""
    echo -e "${YELLOW}📊 To view logs:${NC}"
    echo -e "  docker compose -f $COMPOSE_FILE logs -f"
    echo -e ""
    echo -e "${YELLOW}🛑 To stop services:${NC}"
    echo -e "  docker compose -f $COMPOSE_FILE down"
fi